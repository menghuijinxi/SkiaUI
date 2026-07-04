#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "d3d_context.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class SkCanvas;
class SkSurface;

#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
class GrDirectContext;
#endif

class D3DRenderer {
public:
    using GaneshDrawCallback = std::function<void(SkCanvas&, int, int)>;
    using CpuDrawCallback = std::function<bool(uint32_t*, int, int, size_t)>;

    explicit D3DRenderer(COLORREF background);
    D3DRenderer(COLORREF background, bool retainGaneshFrames);
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    bool initialize(const D3DRenderContext& context);
    void reset();
    bool render(HWND hwnd,
                int width,
                int height,
                const GaneshDrawCallback& drawGanesh,
                const CpuDrawCallback& drawCpu);
    bool tried() const;
    bool ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class D3DPresenter {
public:
    using GaneshDrawCallback = D3DRenderer::GaneshDrawCallback;
    using CpuDrawCallback = D3DRenderer::CpuDrawCallback;

    explicit D3DPresenter(COLORREF background);
    D3DPresenter(COLORREF background, bool retainGaneshFrames);
    ~D3DPresenter();

    D3DPresenter(const D3DPresenter&) = delete;
    D3DPresenter& operator=(const D3DPresenter&) = delete;

    void reset();
    bool render(HWND hwnd,
                int width,
                int height,
                const GaneshDrawCallback& drawGanesh,
                const CpuDrawCallback& drawCpu);
    bool tried() const;
    bool ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
