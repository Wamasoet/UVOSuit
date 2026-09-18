// Copyright (c) 2026 [Wamasoet]
// Ultimate VR Optics Suite (UVOSuit) - Vignette Layer

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>      // Required for uint32_t

using Microsoft::WRL::ComPtr;

/**
 * @brief Handles the initialization and rendering of the vignette effect using DirectX 12.
 */
class VignetteRendererDX12 {
public:
    VignetteRendererDX12() = default;
    ~VignetteRendererDX12();

    /**
     * @brief Initializes the root signature, pipeline state, and descriptor heaps.
     * @param device Pointer to the D3D12 device.
     * @return true if initialization was successful, false otherwise.
     */
    bool Initialize(ID3D12Device* device);

    /**
     * @brief Executes the render pass for the vignette effect.
     * @param commandQueue Pointer to the D3D12 command queue.
     * @param targetTexture The resource texture to render onto.
     * @param isLeftEye True if rendering for the left eye.
     * @param arrayIndex Target texture array slice.
     * @param rectX Viewport/Scissor rectangle X coordinate.
     * @param rectY Viewport/Scissor rectangle Y coordinate.
     * @param rectW Viewport/Scissor rectangle width.
     * @param rectH Viewport/Scissor rectangle height.
     * @param edge_outer Outer edge intensity/position.
     * @param edge_inner Inner edge intensity/position.
     * @param edge_top Top edge masking parameter.
     * @param edge_bottom Bottom edge masking parameter.
     * @param softness Vignette transition softness.
     * @param corner_radius Radius for rounded corners.
     */
    void Render(ID3D12CommandQueue* commandQueue, ID3D12Resource* targetTexture, bool isLeftEye, uint32_t arrayIndex,
        int rectX, int rectY, int rectW, int rectH,
        float edge_outer, float edge_inner, float edge_top, float edge_bottom,
        float softness, float corner_radius);

private:
    bool m_initialized = false;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    static const UINT BUFFER_COUNT = 8;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[BUFFER_COUNT];
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[BUFFER_COUNT];
    UINT64 m_currentFenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    UINT m_bufferIndex = 0;
};