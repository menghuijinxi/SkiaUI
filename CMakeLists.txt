cmake_minimum_required(VERSION 3.24)

project(SkiaRelayDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-skia CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)

option(SKIATEST_USE_SKIA_D3D "Render SkiaLayerDesk through Skia Ganesh D3D12 backend" ON)

add_executable(SkiaRelayDesk WIN32
    src/main.cpp
)

add_executable(SkiaLayerDesk WIN32
    src/layer_desk.cpp
    src/d3d_presenter.cpp
)

add_executable(RmlLayerDesk WIN32
    src/rml_layer_desk.cpp
    src/d3d_presenter.cpp
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

target_link_libraries(RmlLayerDesk PRIVATE
    RmlUi::RmlUi
    unofficial::skia::skia
    unofficial::skia::modules::svg
    user32
    gdi32
    dwmapi
    ole32
    shcore
    shell32
    windowscodecs
    d3d12
    dxgi
    dxguid
    dbghelp
)

target_compile_definitions(RmlLayerDesk PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(RmlLayerDesk PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
endif()

add_custom_command(TARGET RmlLayerDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RmlLayerDesk>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/rml_layer_desk.rml"
            "$<TARGET_FILE_DIR:RmlLayerDesk>/assets/rml_layer_desk.rml"
)

if(SKIATEST_USE_SKIA_D3D)
    target_compile_definitions(SkiaLayerDesk PRIVATE SKIATEST_USE_SKIA_D3D=1)
    target_compile_definitions(RmlLayerDesk PRIVATE SKIATEST_USE_SKIA_D3D=1)
endif()
