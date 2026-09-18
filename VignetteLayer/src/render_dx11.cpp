// Copyright (c) 2026 [Wamasoet]
// Ultimate VR Optics Suite (UVOSuit) - Vignette Layer

#include "render_dx11.hpp"
#include <iostream>
#include "VignetteVS.h"
#include "VignettePS.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

VignetteRendererDX11::~VignetteRendererDX11() {}

bool VignetteRendererDX11::Initialize(ID3D11Device* device) {
    if (m_initialized) return true;
    if (!device) return false;

    // Create the vertex shader directly from the compiled bytecode array
    if (FAILED(device->CreateVertexShader(g_VignetteVS, sizeof(g_VignetteVS), nullptr, &m_vertexShader))) {
        return false;
    }

    // Create the pixel shader directly from the compiled bytecode array
    if (FAILED(device->CreatePixelShader(g_VignettePS, sizeof(g_VignettePS), nullptr, &m_pixelShader))) {
        return false;
    }

    // Create Constant Buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = sizeof(VignetteConstantBuffer);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

    // Configure Blend State (Transparency settings)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, &m_blendState);

    // Disable Z-buffer (render over everything as an overlay)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dsDesc, &m_depthState);

    // Configure Rasterizer State
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rsDesc, &m_rasterizerState);

    m_initialized = true;
    return true;
}

void VignetteRendererDX11::Render(ID3D11DeviceContext* context, ID3D11Texture2D* targetTexture, bool isLeftEye, uint32_t arrayIndex,
    int rectX, int rectY, int rectW, int rectH,
    float edge_outer, float edge_inner, float edge_top, float edge_bottom,
    float softness, float corner_radius) {
    if (!m_initialized || !context || !targetTexture) return;

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    D3D11_TEXTURE2D_DESC texDesc;
    targetTexture->GetDesc(&texDesc);

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};

    // CRITICAL FIX: Check if the target texture is a texture array (e.g., stereo VR layers)
    if (texDesc.ArraySize > 1) {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.FirstArraySlice = arrayIndex; // Target the specific eye array slice
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.MipSlice = 0;
    }
    else {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
    }

    // Resolve typeless formats to specific renderable formats
    switch (texDesc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: rtvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    default: rtvDesc.Format = texDesc.Format; break;
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateRenderTargetView(targetTexture, &rtvDesc, &rtv))) return;

    // Map and update the vignette parameters to the GPU
    D3D11_MAPPED_SUBRESOURCE mappedRes;
    if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedRes))) {
        VignetteConstantBuffer* data = static_cast<VignetteConstantBuffer*>(mappedRes.pData);
        data->edge_outer = edge_outer;
        data->edge_inner = edge_inner;
        data->edge_top = edge_top;
        data->edge_bottom = edge_bottom;
        data->softness = softness;
        data->corner_radius = corner_radius;
        data->isLeftEye = isLeftEye ? 1.0f : 0.0f;
        data->padding = 0.0f;
        context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Backup existing render states to avoid breaking the game's rendering pipeline
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    // Set our texture as the active render target
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

    // Apply render states
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    context->OMSetBlendState(m_blendState.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthState.Get(), 0);
    context->RSSetState(m_rasterizerState.Get());

    // Render strictly within the bounds of the current eye viewport
    D3D11_VIEWPORT vp = { static_cast<float>(rectX), static_cast<float>(rectY), static_cast<float>(rectW), static_cast<float>(rectH), 0.0f, 1.0f };
    context->RSSetViewports(1, &vp);

    // Draw a single full-screen triangle
    context->Draw(3, 0);

    // Restore previous render states seamlessly
    context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
}