// D3D12 half of the presentation bridge. See uwp_present.h for why there is no Vulkan surface.
//
// Shape taken from the one console port that ships this (Cosmo/gow3_xbox, xbox/graphics/
// present_bridge.cpp): CreateSwapChainForCoreWindow with R8G8B8A8_UNORM and FLIP_DISCARD, a single
// shared presentation image, a direct queue doing one CopyResource and a Present per frame.
//
// Compiled by clang-cl (no /ZW here - this is plain C++), so it lives in the same gate as uwp.c.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include "uwp_present.h"

using Microsoft::WRL::ComPtr;

struct UwpPresenter {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> source;  // the Vulkan image, opened in D3D12
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool presented_once = false;   // back buffer needs COMMON -> PRESENT the first time
};

namespace {

// The CoreWindow the host handed over. Held as the raw ABI pointer for the process lifetime: the
// view owns the window, and this only borrows it for CreateSwapChainForCoreWindow.
IUnknown *g_core_window = nullptr;

bool ok(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    fprintf(stderr, "[present] %s failed (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
    return false;
}

}  // namespace

extern "C" bool uwp_present_set_core_window(void *core_window_abi) {
    if (!core_window_abi) return false;
    IUnknown *window = static_cast<IUnknown *>(core_window_abi);
    // The presenter outlives the call and hands the pointer to DXGI, so it takes its own
    // reference rather than borrowing one from the view.
    window->AddRef();
    if (g_core_window) g_core_window->Release();
    g_core_window = window;
    return true;
}

extern "C" UwpPresenter *uwp_present_create(uint32_t width, uint32_t height) {
    if (!g_core_window) {
        fprintf(stderr, "[present] no CoreWindow: call uwp_present_set_core_window first\n");
        return nullptr;
    }

    UwpPresenter *p = new UwpPresenter();
    p->width = width;
    p->height = height;

    // Presentation gets its own device rather than sharing the Vulkan device's: the shared image is
    // opened by handle, which needs no device affinity, and owning the device keeps the queue and
    // fence lifetimes local.
    ComPtr<IDXGIFactory4> factory;
    if (!ok(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) {
        delete p;
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (!ok(factory->EnumAdapters1(0, &adapter), "EnumAdapters1")) {
        delete p;
        return nullptr;
    }
    if (!ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p->device)),
            "D3D12CreateDevice")) {
        delete p;
        return nullptr;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!ok(p->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&p->queue)), "CreateCommandQueue")) {
        delete p;
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGISwapChain1> chain;
    if (!ok(factory->CreateSwapChainForCoreWindow(p->queue.Get(), g_core_window, &desc, nullptr, &chain),
            "CreateSwapChainForCoreWindow")) {
        delete p;
        return nullptr;
    }
    if (!ok(chain.As(&p->chain), "IDXGISwapChain3")) {
        delete p;
        return nullptr;
    }

    if (!ok(p->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&p->allocator)),
            "CreateCommandAllocator")) {
        delete p;
        return nullptr;
    }
    if (!ok(p->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&p->list)),
            "CreateCommandList")) {
        delete p;
        return nullptr;
    }
    p->list->Close();  // created open; the first frame resets it

    if (!ok(p->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence)), "CreateFence")) {
        delete p;
        return nullptr;
    }
    // A manual-reset event, waited on once per frame after the copy is queued.
    p->fence_event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

    fprintf(stderr, "[present] CoreWindow swapchain %ux%u, %u buffers\n", width, height, desc.BufferCount);
    return p;
}

extern "C" bool uwp_present_attach_vulkan_image(UwpPresenter *p, void *nt_handle) {
    if (!p || !nt_handle) return false;
    // The Vulkan allocation must have been created with
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT for this to be the same memory.
    HRESULT hr = p->device->OpenSharedHandle(static_cast<HANDLE>(nt_handle), IID_PPV_ARGS(&p->source));
    if (!ok(hr, "OpenSharedHandle(Vulkan image)")) return false;
    fprintf(stderr, "[present] attached the shared presentation image\n");
    return true;
}

extern "C" bool uwp_present_frame(UwpPresenter *p, bool vsync) {
    if (!p || !p->source) return false;

    const UINT index = p->chain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backbuffer;
    if (!ok(p->chain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)), "GetBuffer")) return false;

    if (!ok(p->allocator->Reset(), "Reset(allocator)")) return false;
    if (!ok(p->list->Reset(p->allocator.Get(), nullptr), "Reset(list)")) return false;

    // First present: the back buffer has never been a present target before.
    if (!p->presented_once) {
        D3D12_RESOURCE_BARRIER to_copy = {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = backbuffer.Get();
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        p->list->ResourceBarrier(1, &to_copy);
    }

    // The Vulkan side leaves the image in a copy-source state; a barrier there is the renderer's
    // job, because only it knows the image's layout history.
    p->list->CopyResource(backbuffer.Get(), p->source.Get());

    D3D12_RESOURCE_BARRIER to_present = {};
    to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_present.Transition.pResource = backbuffer.Get();
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    p->list->ResourceBarrier(1, &to_present);

    if (!ok(p->list->Close(), "Close(list)")) return false;

    ID3D12CommandList *lists[] = { p->list.Get() };
    p->queue->ExecuteCommandLists(1, lists);

    // Wait for the copy before returning: the caller reuses the Vulkan image on the very next
    // frame, and the whole point of a single shared image is that there is nowhere else to put it.
    const uint64_t value = ++p->fence_value;
    if (!ok(p->queue->Signal(p->fence.Get(), value), "Signal(fence)")) return false;
    if (p->fence->GetCompletedValue() < value) {
        if (!ok(p->fence->SetEventOnCompletion(value, p->fence_event), "SetEventOnCompletion")) return false;
        WaitForSingleObjectEx(p->fence_event, INFINITE, FALSE);
    }

    if (!ok(p->chain->Present(vsync ? 1 : 0, 0), "Present")) return false;
    p->presented_once = true;
    return true;
}

extern "C" bool uwp_present_resize(UwpPresenter *p, uint32_t width, uint32_t height) {
    if (!p || !p->chain) return false;
    if (width == p->width && height == p->height) return true;
    p->source.Reset();  // the shared image belongs to the renderer; it re-attaches after resize
    if (!ok(p->chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers")) return false;
    p->width = width;
    p->height = height;
    p->presented_once = false;
    fprintf(stderr, "[present] resized to %ux%u\n", width, height);
    return true;
}

extern "C" void uwp_present_destroy(UwpPresenter *p) {
    if (!p) return;
    if (p->fence_event) CloseHandle(p->fence_event);
    delete p;
}
