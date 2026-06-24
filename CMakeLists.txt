cmake_minimum_required(VERSION 3.24)

project(SkiaRelayDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)

option(SKIATEST_USE_SKIA_D3D "Render SkiaLayerDesk through Skia Ganesh D3D12 backend" ON)

add_executable(SkiaRelayDesk WIN32
    src/main.cpp
)

add_executable(SkiaLayerDesk WIN32
    src/layer_desk.cpp
)

foreach(app_target IN ITEMS SkiaRelayDesk SkiaLayerDesk)
    target_link_libraries(${app_target} PRIVATE
        unofficial::skia::skia
        unofficial::skia::modules::svg
        yoga::yogacore
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        dbghelp
        usp10
    )

    target_compile_definitions(${app_target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(${app_target} PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()
endforeach()

if(SKIATEST_USE_SKIA_D3D)
    target_compile_definitions(SkiaLayerDesk PRIVATE SKIATEST_USE_SKIA_D3D=1)
endif()
