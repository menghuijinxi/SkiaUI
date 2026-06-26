#include "d3d_presenter.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DDirectContext.h"
#include "include/gpu/GpuTypes.h"
#endif

#include <algorithm>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSwapBufferCount = 2;
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

constexpr UINT alignTo(UINT value, UINT alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
class SimpleD3DAlloc final : public GrD3DAlloc {};

class SimpleD3DMemoryAllocator final : public GrD3DMemoryAllocator {
public:
    explicit SimpleD3DMemoryAllocator(ID3D12Device* device) : device_(device) {}

    gr_cp<ID3D12Resource> createResource(D3D12_HEAP_TYPE heapType,
                                         const D3D12_RESOURCE_DESC* desc,
                                         D3D12_RESOURCE_STATES initialResourceState,
                                         sk_sp<GrD3DAlloc>* allocation,
                                         const D3D12_CLEAR_VALUE* clearValue) override {
        if (!device_ || !desc) {
            return {};
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = heapType;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        ID3D12Resource* resource = nullptr;
        if (FAILED(device_->CreateCommittedResource(&heapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    desc,
                                                    initialResourceState,
                                                    clearValue,
                                                    IID_PPV_ARGS(&resource)))) {
            return {};
        }
        if (allocation) {
            *allocation = sk_make_sp<SimpleD3DAlloc>();
        }
        return gr_cp<ID3D12Resource>(resource);
    }

    gr_cp<ID3D12Resource> createAliasingResource(sk_sp<GrD3DAlloc>&,
                                                uint64_t,
                                                const D3D12_RESOURCE_DESC*,
                                                D3D12_RESOURCE_STATES,
                                                const D3D12_CLEAR_VALUE*) override {
        return {};
    }

private:
    ComPtr<ID3D12Device> device_;
};
#endif

}  // namespace

struct D3DPresenter::Impl {
    explicit Impl(COLORREF background, bool retainGaneshFrames)
        : background_(background), retainGaneshFrames_(retainGaneshFrames) {}
    ~Impl() {
        reset();
    }

    COLORREF background_ = RGB(7, 12, 18);
    bool retainGaneshFrames_ = false;
    bool tried = false;
    bool ready = false;
    int swapWidth = 0;
    int swapHeight = 0;
    ComPtr<IDXGIFactory4> dxgiFactory;
    ComPtr<IDXGIAdapter1> dxgiAdapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> uploadBuffer;
    UINT uploadRowPitch = 0;
    uint64_t uploadBufferSize = 0;
    uint8_t* uploadMapped = nullptr;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
    sk_sp<GrD3DMemoryAllocator> allocator;
    sk_sp<GrDirectContext> grContext;
    sk_sp<SkImage> lastGaneshFrame;
    int lastGaneshWidth = 0;
    int lastGaneshHeight = 0;
#endif

    bool init(HWND hwnd, int width, int height) {
        tried = true;
        width = std::max(1, width);
        height = std::max(1, height);

        UINT factoryFlags = 0;
        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&dxgiFactory)))) {
            reset();
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
            reset();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
            reset();
            return false;
        }

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&commandAllocator)))) {
            reset();
            return false;
        }

        if (FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             commandAllocator.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&commandList)))) {
            reset();
            return false;
        }
        commandList->Close();

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            reset();
            return false;
        }

        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            reset();
            return false;
        }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        allocator = sk_make_sp<SimpleD3DMemoryAllocator>(device.Get());

        GrD3DBackendContext backendContext{};
        backendContext.fAdapter.retain(dxgiAdapter.Get());
        backendContext.fDevice.retain(device.Get());
        backendContext.fQueue.retain(queue.Get());
        backendContext.fMemoryAllocator = allocator;
        grContext = GrDirectContexts::MakeD3D(backendContext);
        if (!grContext) {
            allocator.reset();
        }
#endif

        if (!createSwapChain(hwnd, width, height)) {
            reset();
            return false;
        }
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (!grContext && !createUploadBuffer(width, height)) {
            reset();
            return false;
        }
#else
        if (!createUploadBuffer(width, height)) {
            reset();
            return false;
        }
#endif

        ready = true;
        return true;
    }

    void reset() {
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (grContext) {
            grContext->flushAndSubmit(GrSyncCpu::kNo);
        }
#endif
        waitForGpu();
        releaseUploadBuffer();
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        lastGaneshFrame.reset();
        lastGaneshWidth = 0;
        lastGaneshHeight = 0;
        if (grContext) {
            grContext->releaseResourcesAndAbandonContext();
        }
        grContext.reset();
        allocator.reset();
#endif
        swapChain.Reset();
        commandList.Reset();
        commandAllocator.Reset();
        fence.Reset();
        queue.Reset();
        device.Reset();
        dxgiAdapter.Reset();
        dxgiFactory.Reset();
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        fenceValue = 0;
        swapWidth = 0;
        swapHeight = 0;
        ready = false;
    }

    bool waitForGpu() {
        if (!queue || !fence || !fenceEvent) {
            return true;
        }

        const UINT64 value = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), value))) {
            return false;
        }
        if (fence->GetCompletedValue() >= value) {
            return true;
        }
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) {
            return false;
        }
        WaitForSingleObject(fenceEvent, INFINITE);
        return true;
    }

    void releaseUploadBuffer() {
        if (uploadBuffer && uploadMapped) {
            uploadBuffer->Unmap(0, nullptr);
        }
        uploadMapped = nullptr;
        uploadBuffer.Reset();
        uploadRowPitch = 0;
        uploadBufferSize = 0;
    }

    bool createUploadBuffer(int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        const UINT rowPitch = alignTo(static_cast<UINT>(width) * sizeof(uint32_t),
                                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * static_cast<uint64_t>(height);
        if (uploadBuffer && uploadMapped && uploadRowPitch == rowPitch && uploadBufferSize >= uploadSize) {
            return true;
        }

        releaseUploadBuffer();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = uploadSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(&heapProps,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr,
                                                   IID_PPV_ARGS(&uploadBuffer)))) {
            return false;
        }

        D3D12_RANGE noRead{0, 0};
        void* mapped = nullptr;
        if (FAILED(uploadBuffer->Map(0, &noRead, &mapped))) {
            releaseUploadBuffer();
            return false;
        }

        uploadMapped = static_cast<uint8_t*>(mapped);
        uploadRowPitch = rowPitch;
        uploadBufferSize = uploadSize;
        return true;
    }

    void setSwapChainBackground() {
        const DXGI_RGBA background{
            GetRValue(background_) / 255.0f,
            GetGValue(background_) / 255.0f,
            GetBValue(background_) / 255.0f,
            1.0f,
        };
        swapChain->SetBackgroundColor(&background);
    }

    bool createSwapChain(HWND hwnd, int width, int height) {
        if (!dxgiFactory || !queue) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(std::max(1, width));
        desc.Height = static_cast<UINT>(std::max(1, height));
        desc.Format = kSwapFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kSwapBufferCount;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = 0;

        ComPtr<IDXGISwapChain1> swapChain1;
        const HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(queue.Get(),
                                                               hwnd,
                                                               &desc,
                                                               nullptr,
                                                               nullptr,
                                                               &swapChain1);
        if (FAILED(hr) || FAILED(swapChain1.As(&swapChain))) {
            return false;
        }

        dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        setSwapChainBackground();
        swapWidth = std::max(1, width);
        swapHeight = std::max(1, height);
        return true;
    }

    bool resizeSwapChain(int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!swapChain) {
            return false;
        }
        if (width == swapWidth && height == swapHeight) {
            return true;
        }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (grContext) {
            grContext->flushAndSubmit(GrSyncCpu::kNo);
        }
#endif
        waitForGpu();
        releaseUploadBuffer();

        const HRESULT hr = swapChain->ResizeBuffers(kSwapBufferCount,
                                                    static_cast<UINT>(width),
                                                    static_cast<UINT>(height),
                                                    kSwapFormat,
                                                    0);
        if (FAILED(hr)) {
            reset();
            return false;
        }

        swapWidth = width;
        swapHeight = height;
        setSwapChainBackground();
        return true;
    }

    bool presentSwapChain() {
        if (!swapChain) {
            return false;
        }

        DXGI_PRESENT_PARAMETERS params{};
        return SUCCEEDED(swapChain->Present1(0, 0, &params));
    }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
    SkColor backgroundSkColor() const {
        return SkColorSetRGB(GetRValue(background_), GetGValue(background_), GetBValue(background_));
    }

    sk_sp<SkImage> renderGaneshOffscreenImage(int width,
                                              int height,
                                              const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        if (!grContext || !drawGanesh) {
            return nullptr;
        }

        const SkImageInfo imageInfo = SkImageInfo::Make(width,
                                                        height,
                                                        kBGRA_8888_SkColorType,
                                                        kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(grContext.get(),
                                                            skgpu::Budgeted::kNo,
                                                            imageInfo,
                                                            0,
                                                            kTopLeft_GrSurfaceOrigin,
                                                            nullptr);
        if (!surface) {
            return nullptr;
        }

        drawGanesh(*surface->getCanvas(), width, height);
        return surface->makeImageSnapshot();
    }

    void rememberGaneshFrame(sk_sp<SkImage> image, int width, int height) {
        if (!image) {
            return;
        }
        lastGaneshFrame = std::move(image);
        lastGaneshWidth = std::max(1, width);
        lastGaneshHeight = std::max(1, height);
    }

    bool transitionToPresent(ID3D12Resource* resource) {
        if (!device || !queue || !resource) {
            return false;
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)))) {
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&list)))) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);

        if (FAILED(list->Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        return true;
    }

    sk_sp<SkSurface> wrapCurrentBackBuffer(int width,
                                           int height,
                                           ComPtr<ID3D12Resource>& backBuffer,
                                           GrBackendRenderTarget& backendTarget) {
        if (!swapChain || !grContext) {
            return nullptr;
        }

        const UINT bufferIndex = swapChain->GetCurrentBackBufferIndex();
        if (FAILED(swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backBuffer)))) {
            return nullptr;
        }

        GrD3DTextureResourceInfo textureInfo(backBuffer.Get(),
                                             nullptr,
                                             D3D12_RESOURCE_STATE_PRESENT,
                                             kSwapFormat,
                                             1,
                                             1,
                                             DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN);
        backendTarget = GrBackendRenderTargets::MakeD3D(width, height, textureInfo);
        return SkSurfaces::WrapBackendRenderTarget(grContext.get(),
                                                   backendTarget,
                                                   kTopLeft_GrSurfaceOrigin,
                                                   kBGRA_8888_SkColorType,
                                                   nullptr,
                                                   nullptr);
    }

    bool presentGaneshSurface(sk_sp<SkSurface> surface,
                              ID3D12Resource* backBuffer,
                              GrBackendRenderTarget& backendTarget) {
        if (!surface || !backBuffer || !grContext) {
            return false;
        }

        grContext->flushAndSubmit(surface.get());
        surface.reset();

        if (!transitionToPresent(backBuffer)) {
            return false;
        }
        GrBackendRenderTargets::SetD3DResourceState(&backendTarget, D3D12_RESOURCE_STATE_PRESENT);
        return presentSwapChain();
    }

    bool renderPreparedGaneshFrameToSwapChain(int width, int height, sk_sp<SkImage> preparedFrame) {
        if (!preparedFrame || !resizeSwapChain(width, height)) {
            return false;
        }

        ComPtr<ID3D12Resource> backBuffer;
        GrBackendRenderTarget backendTarget;
        sk_sp<SkSurface> surface = wrapCurrentBackBuffer(width, height, backBuffer, backendTarget);
        if (!surface) {
            return false;
        }

        surface->getCanvas()->drawImage(preparedFrame, 0.0f, 0.0f);
        if (!presentGaneshSurface(surface, backBuffer.Get(), backendTarget)) {
            return false;
        }
        rememberGaneshFrame(std::move(preparedFrame), width, height);
        return true;
    }

    bool presentLastGaneshFrameAtSize(int width, int height) {
        if (!lastGaneshFrame || !resizeSwapChain(width, height)) {
            return false;
        }

        ComPtr<ID3D12Resource> backBuffer;
        GrBackendRenderTarget backendTarget;
        sk_sp<SkSurface> surface = wrapCurrentBackBuffer(width, height, backBuffer, backendTarget);
        if (!surface) {
            return false;
        }

        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(backgroundSkColor());
        const float copyWidth = static_cast<float>(std::min(width, lastGaneshWidth));
        const float copyHeight = static_cast<float>(std::min(height, lastGaneshHeight));
        if (copyWidth > 0.0f && copyHeight > 0.0f) {
            const SkRect src = SkRect::MakeWH(copyWidth, copyHeight);
            const SkRect dst = SkRect::MakeWH(copyWidth, copyHeight);
            canvas->drawImageRect(lastGaneshFrame,
                                  src,
                                  dst,
                                  SkSamplingOptions(),
                                  nullptr,
                                  SkCanvas::kStrict_SrcRectConstraint);
        }
        return presentGaneshSurface(surface, backBuffer.Get(), backendTarget);
    }

    bool renderRetainedThenPreparedGaneshFrame(int width,
                                               int height,
                                               const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        if (lastGaneshFrame) {
            (void)presentLastGaneshFrameAtSize(width, height);
        }
        sk_sp<SkImage> preparedFrame = renderGaneshOffscreenImage(width, height, drawGanesh);
        if (!preparedFrame) {
            return false;
        }
        return renderPreparedGaneshFrameToSwapChain(width, height, std::move(preparedFrame));
    }

    bool renderDirectGaneshFrame(int width, int height, const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        ComPtr<ID3D12Resource> backBuffer;
        GrBackendRenderTarget backendTarget;
        sk_sp<SkSurface> surface = wrapCurrentBackBuffer(width, height, backBuffer, backendTarget);
        if (!surface) {
            return false;
        }

        drawGanesh(*surface->getCanvas(), width, height);
        return presentGaneshSurface(surface, backBuffer.Get(), backendTarget);
    }

    bool renderOffscreenGaneshFrame(int width, int height, const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        sk_sp<SkImage> preparedFrame = renderGaneshOffscreenImage(width, height, drawGanesh);
        if (!preparedFrame) {
            return false;
        }
        return renderPreparedGaneshFrameToSwapChain(width, height, std::move(preparedFrame));
    }

    bool shouldRetainEveryGaneshFrame() const {
        return retainGaneshFrames_;
    }

    bool renderStableSizeGaneshD3D(int width, int height, const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        if (shouldRetainEveryGaneshFrame()) {
            return renderOffscreenGaneshFrame(width, height, drawGanesh);
        }
        return renderDirectGaneshFrame(width, height, drawGanesh);
    }

    bool renderResizedGaneshD3D(int width, int height, const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        if (lastGaneshFrame) {
            return renderRetainedThenPreparedGaneshFrame(width, height, drawGanesh);
        }
        return renderOffscreenGaneshFrame(width, height, drawGanesh);
    }

    bool renderSkiaGaneshD3D(int width, int height, const D3DPresenter::GaneshDrawCallback& drawGanesh) {
        if (!grContext || !drawGanesh) {
            return false;
        }

        if (width != swapWidth || height != swapHeight) {
            return renderResizedGaneshD3D(width, height, drawGanesh);
        }
        return renderStableSizeGaneshD3D(width, height, drawGanesh);
    }
#else
    bool renderSkiaGaneshD3D(int, int, const D3DPresenter::GaneshDrawCallback&) {
        return false;
    }
#endif

    bool copyCpuSurfaceToUpload(int width, int height, const D3DPresenter::CpuDrawCallback& drawCpu) {
        if (!uploadMapped || !drawCpu) {
            return false;
        }
        if (!drawCpu(reinterpret_cast<uint32_t*>(uploadMapped), width, height, uploadRowPitch)) {
            return false;
        }
        return true;
    }

    bool renderCpuUpload(int width, int height, const D3DPresenter::CpuDrawCallback& drawCpu) {
        if (!createUploadBuffer(width, height) || !copyCpuSurfaceToUpload(width, height, drawCpu)) {
            return false;
        }

        ComPtr<ID3D12Resource> backBuffer;
        const UINT bufferIndex = swapChain->GetCurrentBackBufferIndex();
        if (FAILED(swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backBuffer)))) {
            return false;
        }

        if (FAILED(commandAllocator->Reset()) ||
            FAILED(commandList->Reset(commandAllocator.Get(), nullptr))) {
            return false;
        }

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toCopy.Transition.pResource = backBuffer.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = backBuffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = kSwapFormat;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;
        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER toPresent = toCopy;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &toPresent);

        if (FAILED(commandList->Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = {commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!waitForGpu()) {
            reset();
            return false;
        }

        if (!presentSwapChain()) {
            reset();
            return false;
        }
        return true;
    }

    bool render(HWND hwnd,
                int width,
                int height,
                const D3DPresenter::GaneshDrawCallback& drawGanesh,
                const D3DPresenter::CpuDrawCallback& drawCpu) {
        if (width <= 0 || height <= 0) {
            return false;
        }

        if (!ready) {
            if (tried || !init(hwnd, width, height)) {
                return false;
            }
        }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (renderSkiaGaneshD3D(width, height, drawGanesh)) {
            return true;
        }
#endif

        if (!resizeSwapChain(width, height)) {
            return false;
        }

        return renderCpuUpload(width, height, drawCpu);
    }
};

D3DPresenter::D3DPresenter(COLORREF background) : D3DPresenter(background, false) {}

D3DPresenter::D3DPresenter(COLORREF background, bool retainGaneshFrames)
    : impl_(new Impl(background, retainGaneshFrames)) {}

D3DPresenter::~D3DPresenter() {
    delete impl_;
}

void D3DPresenter::reset() {
    impl_->reset();
}

bool D3DPresenter::render(HWND hwnd,
                          int width,
                          int height,
                          const GaneshDrawCallback& drawGanesh,
                          const CpuDrawCallback& drawCpu) {
    return impl_->render(hwnd, width, height, drawGanesh, drawCpu);
}

bool D3DPresenter::tried() const {
    return impl_->tried;
}

bool D3DPresenter::ready() const {
    return impl_->ready;
}
