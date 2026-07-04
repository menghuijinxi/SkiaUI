#include "d3d_presenter.h"
#include "perf_trace.h"

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
#include <array>
#include <cstring>
#include <memory>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSwapBufferCount = 2;
constexpr int kTransitionFrameCount = 3;
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

constexpr UINT alignTo(UINT value, UINT alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

ComPtr<IDXGIAdapter1> findAdapterForDevice(IDXGIFactory4& factory, ID3D12Device& device) {
    LUID deviceLuid = device.GetAdapterLuid();
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory.EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (desc.AdapterLuid.HighPart == deviceLuid.HighPart &&
            desc.AdapterLuid.LowPart == deviceLuid.LowPart) {
            return adapter;
        }
    }
    return nullptr;
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

struct D3DRenderer::Impl {
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
    struct TransitionFrame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        UINT64 fenceValue = 0;
    };
    std::array<TransitionFrame, kTransitionFrameCount> transitionFrames;
    size_t transitionFrameIndex = 0;
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

    bool initialize(const D3DRenderContext& context) {
        tried = true;
        if (!context.device || !context.queue) {
            releaseResources();
            return false;
        }

        dxgiFactory = context.dxgiFactory;
        dxgiAdapter = context.adapter;
        device = context.device;
        queue = context.queue;

        if (!dxgiFactory && FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)))) {
            releaseResources();
            return false;
        }
        if (!dxgiAdapter) {
            dxgiAdapter = findAdapterForDevice(*dxgiFactory.Get(), *device.Get());
        }

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&commandAllocator)))) {
            releaseResources();
            return false;
        }

        if (FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             commandAllocator.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&commandList)))) {
            releaseResources();
            return false;
        }
        commandList->Close();

        for (TransitionFrame& frame : transitionFrames) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&frame.allocator)))) {
                releaseResources();
                return false;
            }

            if (FAILED(device->CreateCommandList(0,
                                                 D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 frame.allocator.Get(),
                                                 nullptr,
                                                 IID_PPV_ARGS(&frame.commandList)))) {
                releaseResources();
                return false;
            }
            frame.commandList->Close();
            frame.fenceValue = 0;
        }
        transitionFrameIndex = 0;

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            releaseResources();
            return false;
        }

        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            releaseResources();
            return false;
        }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        allocator = sk_make_sp<SimpleD3DMemoryAllocator>(device.Get());

        GrD3DBackendContext backendContext{};
        if (dxgiAdapter) {
            backendContext.fAdapter.retain(dxgiAdapter.Get());
        }
        backendContext.fDevice.retain(device.Get());
        backendContext.fQueue.retain(queue.Get());
        backendContext.fMemoryAllocator = allocator;
        grContext = GrDirectContexts::MakeD3D(backendContext);
        if (!grContext) {
            allocator.reset();
        }
#endif

        ready = true;
        return true;
    }

    bool ensureSwapChain(HWND hwnd, int width, int height) {
        const auto traceStart = perf::Trace::now();
        width = std::max(1, width);
        height = std::max(1, height);

        if (!createSwapChain(hwnd, width, height)) {
            releaseResources();
            return false;
        }
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (!grContext && !createUploadBuffer(width, height)) {
            releaseResources();
            return false;
        }
#else
        if (!createUploadBuffer(width, height)) {
            releaseResources();
            return false;
        }
#endif

        perf::Trace::write("d3d", "create_swap_chain", width, height, perf::Trace::elapsedMs(traceStart));
        return true;
    }

    void releaseResources() {
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (grContext) {
            grContext->flushAndSubmit(GrSyncCpu::kYes);
            grContext->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
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
        for (TransitionFrame& frame : transitionFrames) {
            frame.commandList.Reset();
            frame.allocator.Reset();
            frame.fenceValue = 0;
        }
        transitionFrameIndex = 0;
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

    void reset() {
        releaseResources();
        tried = false;
    }

    bool waitForGpu() {
        const auto traceStart = perf::Trace::now();
        if (!queue || !fence || !fenceEvent) {
            return true;
        }

        const UINT64 value = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), value))) {
            perf::Trace::write("d3d", "wait_gpu_fail_signal", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart));
            return false;
        }
        if (fence->GetCompletedValue() >= value) {
            perf::Trace::write("d3d", "wait_gpu", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart), "already_complete");
            return true;
        }
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) {
            perf::Trace::write("d3d", "wait_gpu_fail_event", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart));
            return false;
        }
        WaitForSingleObject(fenceEvent, INFINITE);
        perf::Trace::write("d3d", "wait_gpu", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart), "blocked");
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
        const auto traceStart = perf::Trace::now();
        width = std::max(1, width);
        height = std::max(1, height);
        if (!swapChain) {
            return false;
        }
        if (width == swapWidth && height == swapHeight) {
            return true;
        }

        const auto flushStart = perf::Trace::now();
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (grContext) {
            grContext->flushAndSubmit(GrSyncCpu::kYes);
            grContext->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
        }
#endif
        perf::Trace::write("d3d", "resize_flush", width, height, perf::Trace::elapsedMs(flushStart));
        const auto waitStart = perf::Trace::now();
        waitForGpu();
        perf::Trace::write("d3d", "resize_wait", width, height, perf::Trace::elapsedMs(waitStart));
        releaseUploadBuffer();

        const auto resizeBuffersStart = perf::Trace::now();
        const HRESULT hr = swapChain->ResizeBuffers(kSwapBufferCount,
                                                    static_cast<UINT>(width),
                                                    static_cast<UINT>(height),
                                                    kSwapFormat,
                                                    0);
        perf::Trace::write("d3d", "resize_buffers", width, height, perf::Trace::elapsedMs(resizeBuffersStart));
        if (FAILED(hr)) {
            reset();
            return false;
        }

        swapWidth = width;
        swapHeight = height;
        setSwapChainBackground();
        perf::Trace::write("d3d", "resize_total", width, height, perf::Trace::elapsedMs(traceStart));
        return true;
    }

    bool presentSwapChain() {
        const auto traceStart = perf::Trace::now();
        if (!swapChain) {
            return false;
        }

        DXGI_PRESENT_PARAMETERS params{};
        const bool ok = SUCCEEDED(swapChain->Present1(0, 0, &params));
        perf::Trace::write("d3d", ok ? "present" : "present_fail", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart));
        return ok;
    }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
    SkColor backgroundSkColor() const {
        return SkColorSetRGB(GetRValue(background_), GetGValue(background_), GetBValue(background_));
    }

    sk_sp<SkImage> renderGaneshOffscreenImage(int width,
                                              int height,
                                              const D3DRenderer::GaneshDrawCallback& drawGanesh) {
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
        if (!retainGaneshFrames_ || !image) {
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

        TransitionFrame& frame = transitionFrames[transitionFrameIndex];
        if (!frame.allocator || !frame.commandList || !fence || !fenceEvent) {
            return false;
        }

        // A command allocator cannot be reset until the GPU has consumed the
        // previous command list that used it. Rotate a few tiny transition lists
        // and only wait when rapid resize/present traffic laps the GPU.
        if (frame.fenceValue != 0 && fence->GetCompletedValue() < frame.fenceValue) {
            if (FAILED(fence->SetEventOnCompletion(frame.fenceValue, fenceEvent))) {
                return false;
            }
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        if (FAILED(frame.allocator->Reset()) ||
            FAILED(frame.commandList->Reset(frame.allocator.Get(), nullptr))) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        frame.commandList->ResourceBarrier(1, &barrier);

        if (FAILED(frame.commandList->Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = {frame.commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), value))) {
            return false;
        }
        frame.fenceValue = value;
        transitionFrameIndex = (transitionFrameIndex + 1) % transitionFrames.size();
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

        GrD3DTextureResourceInfo textureInfo;
        // gr_cp(T*) adopts a COM reference. Keep the swap-chain ComPtr's
        // reference separate and give Skia its own retained reference.
        textureInfo.fResource.retain(backBuffer.Get());
        textureInfo.fResourceState = D3D12_RESOURCE_STATE_PRESENT;
        textureInfo.fFormat = kSwapFormat;
        textureInfo.fSampleCount = 1;
        textureInfo.fLevelCount = 1;
        textureInfo.fSampleQualityPattern = DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN;
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
        const auto traceStart = perf::Trace::now();
        if (!surface || !backBuffer || !grContext) {
            return false;
        }

        const auto flushStart = perf::Trace::now();
        grContext->flushAndSubmit(surface.get());
        perf::Trace::write("d3d", "ganesh_flush_submit", swapWidth, swapHeight, perf::Trace::elapsedMs(flushStart));
        surface.reset();

        const auto transitionStart = perf::Trace::now();
        if (!transitionToPresent(backBuffer)) {
            return false;
        }
        perf::Trace::write("d3d", "transition_present", swapWidth, swapHeight, perf::Trace::elapsedMs(transitionStart));
        GrBackendRenderTargets::SetD3DResourceState(&backendTarget, D3D12_RESOURCE_STATE_PRESENT);
        const bool ok = presentSwapChain();
        perf::Trace::write("d3d", "present_ganesh_total", swapWidth, swapHeight, perf::Trace::elapsedMs(traceStart));
        return ok;
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
                                               const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        if (lastGaneshFrame) {
            (void)presentLastGaneshFrameAtSize(width, height);
        }
        sk_sp<SkImage> preparedFrame = renderGaneshOffscreenImage(width, height, drawGanesh);
        if (!preparedFrame) {
            return false;
        }
        return renderPreparedGaneshFrameToSwapChain(width, height, std::move(preparedFrame));
    }

    bool renderDirectGaneshFrame(int width, int height, const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        const auto traceStart = perf::Trace::now();
        ComPtr<ID3D12Resource> backBuffer;
        GrBackendRenderTarget backendTarget;
        const auto wrapStart = perf::Trace::now();
        sk_sp<SkSurface> surface = wrapCurrentBackBuffer(width, height, backBuffer, backendTarget);
        perf::Trace::write("d3d", "wrap_backbuffer", width, height, perf::Trace::elapsedMs(wrapStart));
        if (!surface) {
            return false;
        }

        const auto drawStart = perf::Trace::now();
        drawGanesh(*surface->getCanvas(), width, height);
        perf::Trace::write("d3d", "draw_ganesh_callback", width, height, perf::Trace::elapsedMs(drawStart));
        const bool ok = presentGaneshSurface(surface, backBuffer.Get(), backendTarget);
        perf::Trace::write("d3d", "render_direct_ganesh_total", width, height, perf::Trace::elapsedMs(traceStart));
        return ok;
    }

    bool renderOffscreenGaneshFrame(int width, int height, const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        sk_sp<SkImage> preparedFrame = renderGaneshOffscreenImage(width, height, drawGanesh);
        if (!preparedFrame) {
            return false;
        }
        return renderPreparedGaneshFrameToSwapChain(width, height, std::move(preparedFrame));
    }

    bool shouldRetainEveryGaneshFrame() const {
        return retainGaneshFrames_;
    }

    bool renderStableSizeGaneshD3D(int width, int height, const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        if (shouldRetainEveryGaneshFrame()) {
            return renderOffscreenGaneshFrame(width, height, drawGanesh);
        }
        return renderDirectGaneshFrame(width, height, drawGanesh);
    }

    bool renderResizedGaneshD3D(int width, int height, const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        if (!shouldRetainEveryGaneshFrame()) {
            if (!resizeSwapChain(width, height)) {
                return false;
            }
            return renderDirectGaneshFrame(width, height, drawGanesh);
        }

        if (lastGaneshFrame) {
            return renderRetainedThenPreparedGaneshFrame(width, height, drawGanesh);
        }
        return renderOffscreenGaneshFrame(width, height, drawGanesh);
    }

    bool renderSkiaGaneshD3D(int width, int height, const D3DRenderer::GaneshDrawCallback& drawGanesh) {
        if (!grContext || !drawGanesh) {
            return false;
        }

        if (width != swapWidth || height != swapHeight) {
            return renderResizedGaneshD3D(width, height, drawGanesh);
        }
        return renderStableSizeGaneshD3D(width, height, drawGanesh);
    }
#else
    bool renderSkiaGaneshD3D(int, int, const D3DRenderer::GaneshDrawCallback&) {
        return false;
    }
#endif

    bool copyCpuSurfaceToUpload(int width, int height, const D3DRenderer::CpuDrawCallback& drawCpu) {
        if (!uploadMapped || !drawCpu) {
            return false;
        }
        if (!drawCpu(reinterpret_cast<uint32_t*>(uploadMapped), width, height, uploadRowPitch)) {
            return false;
        }
        return true;
    }

    bool renderCpuUpload(int width, int height, const D3DRenderer::CpuDrawCallback& drawCpu) {
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
                const D3DRenderer::GaneshDrawCallback& drawGanesh,
                const D3DRenderer::CpuDrawCallback& drawCpu) {
        const auto traceStart = perf::Trace::now();
        if (width <= 0 || height <= 0) {
            return false;
        }

        if (!ready) {
            return false;
        }
        if (!swapChain && !ensureSwapChain(hwnd, width, height)) {
            return false;
        }

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        if (renderSkiaGaneshD3D(width, height, drawGanesh)) {
            perf::Trace::write("d3d", "render_total", width, height, perf::Trace::elapsedMs(traceStart), "ganesh");
            return true;
        }
#endif

        if (!resizeSwapChain(width, height)) {
            return false;
        }

        const bool ok = renderCpuUpload(width, height, drawCpu);
        perf::Trace::write("d3d", "render_total", width, height, perf::Trace::elapsedMs(traceStart), "cpu_upload");
        return ok;
    }
};

D3DRenderer::D3DRenderer(COLORREF background) : D3DRenderer(background, false) {}

D3DRenderer::D3DRenderer(COLORREF background, bool retainGaneshFrames)
    : impl_(std::make_unique<Impl>(background, retainGaneshFrames)) {}

D3DRenderer::~D3DRenderer() = default;

bool D3DRenderer::initialize(const D3DRenderContext& context) {
    return impl_->ready || (!impl_->tried && impl_->initialize(context));
}

void D3DRenderer::reset() {
    impl_->reset();
}

bool D3DRenderer::render(HWND hwnd,
                         int width,
                         int height,
                         const GaneshDrawCallback& drawGanesh,
                         const CpuDrawCallback& drawCpu) {
    return impl_->render(hwnd, width, height, drawGanesh, drawCpu);
}

bool D3DRenderer::tried() const {
    return impl_->tried;
}

bool D3DRenderer::ready() const {
    return impl_->ready;
}

struct D3DPresenter::Impl {
    explicit Impl(COLORREF background, bool retainGaneshFrames)
        : renderer(background, retainGaneshFrames) {}

    D3DContext context;
    D3DRenderer renderer;

    void reset() {
        renderer.reset();
        context.reset();
    }
};

D3DPresenter::D3DPresenter(COLORREF background) : D3DPresenter(background, false) {}

D3DPresenter::D3DPresenter(COLORREF background, bool retainGaneshFrames)
    : impl_(std::make_unique<Impl>(background, retainGaneshFrames)) {}

D3DPresenter::~D3DPresenter() = default;

void D3DPresenter::reset() {
    impl_->reset();
}

bool D3DPresenter::render(HWND hwnd,
                          int width,
                          int height,
                          const GaneshDrawCallback& drawGanesh,
                          const CpuDrawCallback& drawCpu) {
    if (!impl_->context.ready() && !impl_->context.initialize()) {
        return false;
    }
    if (!impl_->renderer.ready() && !impl_->renderer.initialize(impl_->context.renderContext())) {
        return false;
    }
    return impl_->renderer.render(hwnd, width, height, drawGanesh, drawCpu);
}

bool D3DPresenter::tried() const {
    return impl_->context.tried() || impl_->renderer.tried();
}

bool D3DPresenter::ready() const {
    return impl_->context.ready() && impl_->renderer.ready();
}
