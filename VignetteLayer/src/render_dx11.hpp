// Copyright (c) 2026 [Wamasoet]
// Ultimate VR Optics Suite (UVOSuit) - Vignette Layer

#pragma once

#include <d3d11.h>
#include <wrl/client.h> // For smart pointers (ComPtr)
#include <string>
#include <cstdint>      // Required for uint32_t

using Microsoft::WRL::ComPtr;

/**
 * @brief Constant buffer structure mapped directly to the HLSL shader.
 * @note Size must be a multiple of 16 bytes to satisfy DirectX alignment requirements.
 */
struct VignetteConstantBuffer {
    float edge_outer;
    float edge_inner;
    float edge_top;
    float edge_bottom;
    float softness;
    float corner_radius;
    float isLeftEye; // 1.0f for the left eye, 0.0f for the right eye
    float padding;   // Padding to ensure 16-byte memory alignment
};

/**
 * @brief Handles the initialization and rendering of the vignette effect using DirectX 11.
 */
class VignetteRendererDX11 {
public:
    VignetteRendererDX11() = default;
    ~VignetteRendererDX11();

    /**
     * @brief Initializes the D3D11 rendering pipeline, shaders, and buffers.
     * @param device Pointer to the D3D11 device.
     * @return true if initialization was successful, false otherwise.
     */
    bool Initialize(ID3D11Device* device);

    /**
     * @brief Executes the render pass for the vignette effect.
     * @param context Pointer to the D3D11 device context.
     * @param targetTexture The texture to render the vignette onto.
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
    void Render(ID3D11DeviceContext* context, ID3D11Texture2D* targetTexture, bool isLeftEye, uint32_t arrayIndex,
        int rectX, int rectY, int rectW, int rectH,
        float edge_outer, float edge_inner, float edge_top, float edge_bottom,
        float softness, float corner_radius);

private:
    bool m_initialized = false;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11DepthStencilState> m_depthState;
};