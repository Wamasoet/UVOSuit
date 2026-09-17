// Extended for Ultimate VR Optics Suite

#include <vector>
#include <unordered_map>
#include <cassert>
#include <iostream>
#include <windows.h>

#include "layer_shims.hpp"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include "render_dx11.hpp"
#include "render_dx12.hpp"

using Microsoft::WRL::ComPtr;

// =========================================================================
// SHARED MEMORY & CONFIGURATION
// =========================================================================

/**
 * @brief Lens Mask parameters structure.
 * @note This must be strictly identical in both the API Layer and the main plugin.
 */
struct VignetteConfig {
    bool enabled;
    float edge_outer;
    float edge_inner;
    float edge_top;
    float edge_bottom;
    float softness;
    float corner_radius;
};

// =========================================================================
// GLOBAL STATE
// =========================================================================

enum class GraphicsAPI { Unknown, D3D11, D3D12 };
GraphicsAPI g_graphicsAPI = GraphicsAPI::Unknown;

ID3D11Device* g_d3d11Device = nullptr;
ID3D12CommandQueue* g_d3d12CommandQueue = nullptr;

VignetteRendererDX11 g_vignetteRendererDX11;
VignetteRendererDX12 g_vignetteRendererDX12;

/**
 * @brief Container for tracking textures associated with each OpenXR swapchain.
 */
struct SwapchainData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t acquiredIndex = 0;
    std::vector<ID3D11Texture2D*> d3d11Textures;
    std::vector<ID3D12Resource*> d3d12Textures;
};

/// Map linking OpenXR swapchain handles to our internal data
std::unordered_map<XrSwapchain, SwapchainData> g_swapchains;

// IPC Global Pointers
HANDLE g_hMapFile = nullptr;
VignetteConfig* g_vignetteConfig = nullptr;

// =========================================================================
// INTERNAL FUNCTIONS
// =========================================================================

/**
 * @brief Initializes the Shared Memory bridge to receive parameters from the main plugin.
 */
void InitSharedMemory() {
    if (g_vignetteConfig != nullptr) return; // Already initialized

    // Create or open the named file mapping object for inter-process communication (IPC)
    g_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(VignetteConfig),
        "Local\\UVOSuit_Vignette_Data" // Unique bridge identifier
    );

    if (g_hMapFile != nullptr) {
        g_vignetteConfig = (VignetteConfig*)MapViewOfFile(
            g_hMapFile,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(VignetteConfig)
        );
    }
}

// =========================================================================
// OPENXR API SHIMS (HOOKS)
// =========================================================================

/**
 * @brief Hook for xrDestroyInstance.
 * @note IMPORTANT: To allow for multiple instance creation/destruction, the context
 * of the layer must be re-initialized when the instance is being destroyed.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrDestroyInstance(XrInstance instance) {
    PFN_xrDestroyInstance nextLayer_xrDestroyInstance = GetNextLayerFunction(xrDestroyInstance);

    g_swapchains.clear();
    OpenXRLayer::DestroyLayerContext();

    assert(nextLayer_xrDestroyInstance != nullptr);
    return nextLayer_xrDestroyInstance(instance);
}

/**
 * @brief Hook for xrCreateSession. Detects the graphics API being used.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    static PFN_xrCreateSession nextLayer_xrCreateSession = GetNextLayerFunction(xrCreateSession);

    // 1. Initialize the Shared Memory bridge
    InitSharedMemory();

    // 2. Scan the OpenXR structure chain to identify the active graphics binding
    const XrBaseInStructure* next = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next != nullptr) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            g_graphicsAPI = GraphicsAPI::D3D11;
            g_d3d11Device = binding->device;
            g_vignetteRendererDX11.Initialize(g_d3d11Device);
            break;
        }
        else if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
            g_graphicsAPI = GraphicsAPI::D3D12;
            g_d3d12CommandQueue = binding->queue;
            g_vignetteRendererDX12.Initialize(binding->device);
            break;
        }
        next = next->next;
    }

    return nextLayer_xrCreateSession(instance, createInfo, session);
}

/**
 * @brief Hook for xrCreateSwapchain. Tracks texture dimensions.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    static PFN_xrCreateSwapchain nextLayer_xrCreateSwapchain = GetNextLayerFunction(xrCreateSwapchain);
    XrResult result = nextLayer_xrCreateSwapchain(session, createInfo, swapchain);

    if (result == XR_SUCCESS && swapchain != nullptr) {
        // Store texture dimensions for this specific swapchain
        g_swapchains[*swapchain].width = createInfo->width;
        g_swapchains[*swapchain].height = createInfo->height;
    }

    return result;
}

/**
 * @brief Hook for xrEnumerateSwapchainImages. Caches texture resources.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images) {
    static PFN_xrEnumerateSwapchainImages nextLayer_xrEnumerateSwapchainImages = GetNextLayerFunction(xrEnumerateSwapchainImages);
    XrResult result = nextLayer_xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);

    // If the game requests the textures (images != nullptr), cache their addresses
    if (result == XR_SUCCESS && images != nullptr && imageCapacityInput > 0) {
        auto& data = g_swapchains[swapchain];

        if (g_graphicsAPI == GraphicsAPI::D3D11) {
            auto* d3d11Images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            data.d3d11Textures.clear();
            for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                data.d3d11Textures.push_back(d3d11Images[i].texture);
            }
        }
        else if (g_graphicsAPI == GraphicsAPI::D3D12) {
            auto* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            data.d3d12Textures.clear();
            for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                data.d3d12Textures.push_back(d3d12Images[i].texture);
            }
        }
    }

    return result;
}

/**
 * @brief Hook for xrAcquireSwapchainImage. Tracks the currently active texture index.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    static PFN_xrAcquireSwapchainImage nextLayer_xrAcquireSwapchainImage = GetNextLayerFunction(xrAcquireSwapchainImage);
    XrResult result = nextLayer_xrAcquireSwapchainImage(swapchain, acquireInfo, index);

    // Memorize the index of the active texture for rendering
    if (result == XR_SUCCESS && index != nullptr) {
        g_swapchains[swapchain].acquiredIndex = *index;
    }

    return result;
}

/**
 * @brief Hook for xrEndFrame. Injects the custom vignette rendering into the pipeline.
 */
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    static PFN_xrEndFrame nextLayer_xrEndFrame = GetNextLayerFunction(xrEndFrame);

    // 1. Verify vignette is enabled and pointer chains are valid
    if (g_vignetteConfig != nullptr && g_vignetteConfig->enabled &&
        frameEndInfo != nullptr && frameEndInfo->layers != nullptr) {

        // Iterate through composition layers to find the projection layer
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            // Null-pointer guard for the layer array
            if (frameEndInfo->layers[i] != nullptr &&
                frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {

                const auto* projLayer = reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                // Guard against corrupted or empty views
                if (projLayer->views == nullptr) continue;

                // Iterate through the views (0 = Left Eye, 1 = Right Eye)
                for (uint32_t v = 0; v < projLayer->viewCount; ++v) {
                    XrSwapchain swapchain = projLayer->views[v].subImage.swapchain;

                    auto it = g_swapchains.find(swapchain);
                    if (it != g_swapchains.end()) {
                        uint32_t texIndex = it->second.acquiredIndex;
                        bool isLeftEye = (v == 0);

                        if (g_graphicsAPI == GraphicsAPI::D3D11 && g_d3d11Device != nullptr) {
                            if (texIndex < it->second.d3d11Textures.size()) {
                                ID3D11Texture2D* texture = it->second.d3d11Textures[texIndex];

                                if (texture != nullptr) {
                                    ComPtr<ID3D11DeviceContext> context;
                                    g_d3d11Device->GetImmediateContext(&context);

                                    if (context) {
                                        uint32_t arraySlice = projLayer->views[v].subImage.imageArrayIndex;

                                        // Retrieve the specific eye's viewport rectangle
                                        auto rect = projLayer->views[v].subImage.imageRect;

                                        g_vignetteRendererDX11.Render(
                                            context.Get(),
                                            texture,
                                            isLeftEye,
                                            arraySlice,
                                            rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height,
                                            g_vignetteConfig->edge_outer,
                                            g_vignetteConfig->edge_inner,
                                            g_vignetteConfig->edge_top,
                                            g_vignetteConfig->edge_bottom,
                                            g_vignetteConfig->softness,
                                            g_vignetteConfig->corner_radius
                                        );
                                    }
                                }
                            }
                        }
                        else if (g_graphicsAPI == GraphicsAPI::D3D12 && g_d3d12CommandQueue != nullptr) {
                            if (texIndex < it->second.d3d12Textures.size()) {
                                ID3D12Resource* texture = it->second.d3d12Textures[texIndex];

                                if (texture != nullptr) {
                                    uint32_t arraySlice = projLayer->views[v].subImage.imageArrayIndex;
                                    auto rect = projLayer->views[v].subImage.imageRect;

                                    g_vignetteRendererDX12.Render(
                                        g_d3d12CommandQueue, texture, isLeftEye, arraySlice,
                                        rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height,
                                        g_vignetteConfig->edge_outer, g_vignetteConfig->edge_inner,
                                        g_vignetteConfig->edge_top, g_vignetteConfig->edge_bottom,
                                        g_vignetteConfig->softness, g_vignetteConfig->corner_radius
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return nextLayer_xrEndFrame(session, frameEndInfo);
}

#if XR_THISLAYER_HAS_EXTENSIONS
// The following function doesn't exist in the spec, this is just a test for the extension mechanism
XRAPI_ATTR XrResult XRAPI_CALL thisLayer_xrTestMeTEST(XrSession session) {
    (void)session;
    std::cout << "xrTestMe()\n";
    return XR_SUCCESS;
}
#endif

/**
 * @brief Returns the list of implemented function pointers during layer initialization.
 */
std::vector<OpenXRLayer::ShimFunction> ListShims() {
    std::vector<OpenXRLayer::ShimFunction> functions;

    // Core Instance & Session management
    functions.emplace_back("xrDestroyInstance", PFN_xrVoidFunction(thisLayer_xrDestroyInstance));
    functions.emplace_back("xrCreateSession", PFN_xrVoidFunction(thisLayer_xrCreateSession));

    // Swapchain management
    functions.emplace_back("xrCreateSwapchain", PFN_xrVoidFunction(thisLayer_xrCreateSwapchain));
    functions.emplace_back("xrEnumerateSwapchainImages", PFN_xrVoidFunction(thisLayer_xrEnumerateSwapchainImages));
    functions.emplace_back("xrAcquireSwapchainImage", PFN_xrVoidFunction(thisLayer_xrAcquireSwapchainImage));

    // Render loop
    functions.emplace_back("xrEndFrame", PFN_xrVoidFunction(thisLayer_xrEndFrame));

#if XR_THISLAYER_HAS_EXTENSIONS
    if (OpenXRLayer::IsExtensionEnabled("XR_TEST_test_me"))
        functions.emplace_back("xrTestMeTEST", PFN_xrVoidFunction(thisLayer_xrTestMeTEST));
#endif

    return functions;
}