#pragma once

#include <memory>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct IDXGIAdapter1;
struct IDXGIFactory4;

struct D3DRenderContext {
    IDXGIFactory4* dxgiFactory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
};

class D3DContext {
public:
    D3DContext();
    ~D3DContext();

    D3DContext(const D3DContext&) = delete;
    D3DContext& operator=(const D3DContext&) = delete;

    bool initialize();
    void reset();
    bool tried() const;
    bool ready() const;
    D3DRenderContext renderContext() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
