#include "render_dx11.hpp"
#include <d3dcompiler.h>
#include <iostream>

// Link the DirectX shader compiler library
#pragma comment(lib, "d3dcompiler.lib")

// =========================================================================
// EMBEDDED HLSL SHADER SOURCE CODE
// =========================================================================
const std::string VignetteRendererDX11::s_shaderCode = R"(
cbuffer VignetteBuffer : register(b0) {
    float edge_outer;
    float edge_inner;
    float edge_top;
    float edge_bottom;
    float softness;
    float corner_radius;
    float isLeftEye;
    float padding;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VSMain(uint id : SV_VertexID) {
    VS_OUTPUT output;
    // Generate a full-screen triangle using vertex ID
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(VS_OUTPUT input) : SV_Target {
    // 1. Asymmetry: Determine the temporal (outer) and nasal (inner) edges based on the eye
    float left_edge  = (isLeftEye > 0.5) ? edge_outer : edge_inner;
    float right_edge = (isLeftEye > 0.5) ? edge_inner : edge_outer;
    
    // 2. Define the clear window coordinates (areas without vignette masking)
    float2 minBound = float2(left_edge, edge_top);
    float2 maxBound = float2(1.0 - right_edge, 1.0 - edge_bottom);
    
    // 3. Calculate the center and half-size of the bounding box
    float2 boxCenter = (minBound + maxBound) * 0.5;
    float2 boxHalfSize = (maxBound - minBound) * 0.5;
    
    // 4. Clamp corner radius to prevent shape distortion if extreme values are provided
    float max_radius = min(boxHalfSize.x, boxHalfSize.y);
    float radius = min(corner_radius, max_radius);
    
    // 5. Signed Distance Field (SDF) for the rounded rectangle
    float2 p = input.uv - boxCenter;
    float2 d = abs(p) - boxHalfSize + radius;
    
    // dist < 0 inside the window, 0 on the exact edge, and > 0 in the masked area
    float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    
    // 6. Soft lens edge blending
    // Pixels where dist <= 0 become fully transparent (alpha = 0)
    // Pixels where dist > softness become fully opaque (alpha = 1)
    float alpha = smoothstep(0.0, softness + 0.0001, dist);
    
    return float4(0.0, 0.0, 0.0, alpha);
}
)";

VignetteRendererDX11::~VignetteRendererDX11() {}

bool VignetteRendererDX11::Initialize(ID3D11Device* device) {
    if (m_initialized) return true;
    if (!device) return false;

    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    // Compile Vertex Shader
    if (FAILED(D3DCompile(s_shaderCode.c_str(), s_shaderCode.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vsBlob, &errorBlob))) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return false;
    }
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);

    // Compile Pixel Shader
    if (FAILED(D3DCompile(s_shaderCode.c_str(), s_shaderCode.size(), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &psBlob, &errorBlob))) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return false;
    }
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);

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

    // Disable Z-buffer (render over everything)
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
        VignetteConstantBuffer* data = (VignetteConstantBuffer*)mappedRes.pData;
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

    // Set our texture as the render target
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
    D3D11_VIEWPORT vp = { (float)rectX, (float)rectY, (float)rectW, (float)rectH, 0.0f, 1.0f };
    context->RSSetViewports(1, &vp);

    // Draw a single full-screen triangle
    context->Draw(3, 0);

    // Restore previous render states
    context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
}