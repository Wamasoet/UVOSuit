// Copyright (c) 2026 [Wamasoet]
// Ultimate VR Optics Suite (UVOSuit) - Vignette Layer

#include "render_dx12.hpp"
#include <iostream>
#include "VignetteVS.h"
#include "VignettePS.h"

#pragma comment(lib, "d3d12.lib")

VignetteRendererDX12::~VignetteRendererDX12() {
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool VignetteRendererDX12::Initialize(ID3D12Device* device) {
    if (m_initialized) return true;
    if (!device) return false;
    m_device = device;

    // 1. Create Root Signature
    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParam.Constants.ShaderRegister = 0; // register(b0)
    rootParam.Constants.RegisterSpace = 0;
    rootParam.Constants.Num32BitValues = 8; // 8 floats
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = &rootParam;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
        if (error) OutputDebugStringA(static_cast<char*>(error->GetBufferPointer()));
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }

    // 2. Create a pool of Command Allocators for asynchronous rendering
    for (UINT i = 0; i < BUFFER_COUNT; i++) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])))) return false;
        m_fenceValues[i] = 0;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList)))) return false;
    m_commandList->Close();

    // 3. Create synchronization objects (Fences) once during initialization
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) return false;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_currentFenceValue = 0;
    m_bufferIndex = 0;

    // 4. Create Descriptor Heap for Render Target Views (RTV)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;

    m_initialized = true;
    return true;
}

void VignetteRendererDX12::Render(ID3D12CommandQueue* commandQueue, ID3D12Resource* targetTexture, bool isLeftEye, uint32_t arrayIndex,
    int rectX, int rectY, int rectW, int rectH,
    float edge_outer, float edge_inner, float edge_top, float edge_bottom,
    float softness, float corner_radius) {
    if (!m_initialized || !commandQueue || !targetTexture) return;

    D3D12_RESOURCE_DESC texDesc = targetTexture->GetDesc();

    // 1. Resolve RTV format
    DXGI_FORMAT rtvFormat = texDesc.Format;
    switch (texDesc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: rtvFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    }

    // 2. Lazy initialization of the Pipeline State Object (PSO)
    if (!m_pipelineState) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = m_rootSignature.Get();

        // IMPORTANT: Pass bytecodes directly from the included header files
        psoDesc.VS = { g_VignetteVS, sizeof(g_VignetteVS) };
        psoDesc.PS = { g_VignettePS, sizeof(g_VignettePS) };

        D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
        blendDesc.BlendEnable = TRUE;
        blendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.BlendState.RenderTarget[0] = blendDesc;
        psoDesc.SampleMask = UINT_MAX;

        psoDesc.RasterizerState = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP, D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF };
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)))) return;
    }

    // 3. Prepare command list asynchronously
    // Cycle to the next allocator in the ring buffer
    m_bufferIndex = (m_bufferIndex + 1) % BUFFER_COUNT;

    // Wait only if the GPU has not finished processing this specific buffer (rare with 8 buffers)
    if (m_fence->GetCompletedValue() < m_fenceValues[m_bufferIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_bufferIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_commandAllocators[m_bufferIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_pipelineState.Get());

    // 4. Create Render Target View (RTV)
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;

    // Handle Texture2D Arrays (e.g., stereo VR layers)
    if (texDesc.DepthOrArraySize > 1) {
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.FirstArraySlice = arrayIndex;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.MipSlice = 0;
    }
    else {
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(targetTexture, &rtvDesc, rtvHandle);

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // 5. Set Viewport and Scissor Rect
    D3D12_VIEWPORT vp = { static_cast<float>(rectX), static_cast<float>(rectY), static_cast<float>(rectW), static_cast<float>(rectH), 0.0f, 1.0f };
    D3D12_RECT scissor = { rectX, rectY, rectX + rectW, rectY + rectH };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &scissor);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 6. Pass parameters via Root Constants
    float constants[8] = { edge_outer, edge_inner, edge_top, edge_bottom, softness, corner_radius, isLeftEye ? 1.0f : 0.0f, 0.0f };
    m_commandList->SetGraphicsRoot32BitConstants(0, 8, constants, 0);

    // 7. Draw the full-screen triangle
    m_commandList->DrawInstanced(3, 1, 0, 0);
    m_commandList->Close();

    // 8. Execute the command list
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    commandQueue->ExecuteCommandLists(1, ppCommandLists);

    // 9. OPTIMIZED SYNCHRONIZATION
    // Signal the fence from the GPU but do NOT block the CPU. The CPU continues execution immediately.
    m_currentFenceValue++;
    commandQueue->Signal(m_fence.Get(), m_currentFenceValue);
    m_fenceValues[m_bufferIndex] = m_currentFenceValue;
}