#include "d3d_context.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct D3DContext::Impl {
    bool tried = false;
    bool ready = false;
    ComPtr<IDXGIFactory4> dxgiFactory;
    ComPtr<IDXGIAdapter1> dxgiAdapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    bool initialize() {
        tried = true;

        UINT factoryFlags = 0;
        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&dxgiFactory)))) {
            releaseResources();
            return false;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            if (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                                            D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device),
                                            nullptr))) {
                dxgiAdapter = adapter;
                break;
            }
        }

        if (!dxgiAdapter ||
            FAILED(D3D12CreateDevice(dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            releaseResources();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
            releaseResources();
            return false;
        }

        ready = true;
        return true;
    }

    void releaseResources() {
        queue.Reset();
        device.Reset();
        dxgiAdapter.Reset();
        dxgiFactory.Reset();
        ready = false;
    }

    void reset() {
        releaseResources();
        tried = false;
    }

    D3DRenderContext renderContext() const {
        return {
            dxgiFactory.Get(),
            dxgiAdapter.Get(),
            device.Get(),
            queue.Get(),
        };
    }
};

D3DContext::D3DContext() : impl_(std::make_unique<Impl>()) {}

D3DContext::~D3DContext() = default;

bool D3DContext::initialize() {
    return impl_->ready || (!impl_->tried && impl_->initialize());
}

void D3DContext::reset() {
    impl_->reset();
}

bool D3DContext::tried() const {
    return impl_->tried;
}

bool D3DContext::ready() const {
    return impl_->ready;
}

D3DRenderContext D3DContext::renderContext() const {
    return impl_->renderContext();
}
