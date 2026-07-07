cmake_minimum_required(VERSION 3.24)

project(SkiaRelayDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(CMakePackageConfigHelpers)

find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)

option(SKIAUI_BUILD_DEMOS "Build SkiaUI demo applications" ON)
option(SKIAUI_BUILD_SKIA_UI_DESK "Build the SkiaUiDesk demo application" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_TEXT_FILE_DEMO "Build the text file rendering performance demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO "Build the image scroller memory demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_INSTALL_SKIA_UI_DESK "Install the SkiaUiDesk demo application" OFF)
option(SKIAUI_BUILD_TESTS "Build SkiaUI tests" ${BUILD_TESTING})
option(SKIAUI_BUILD_WIN32 "Build the Win32 SkUI platform adapter target" ON)
option(SKIAUI_INSTALL_WIN32 "Install the Win32 SkUI platform adapter target" ON)
option(SKIAUI_BUILD_WIN32_DX12 "Build the Win32/DX12 SkUI host target" ON)
option(SKIAUI_INSTALL_WIN32_DX12 "Install the Win32/DX12 SkUI host target" ON)
option(SKIATEST_USE_SKIA_D3D "Render SkiaLayerDesk through Skia Ganesh D3D12 backend" ON)

if(SKIAUI_BUILD_WIN32_DX12 AND NOT SKIAUI_BUILD_WIN32)
    message(FATAL_ERROR "SKIAUI_BUILD_WIN32_DX12 requires SKIAUI_BUILD_WIN32.")
endif()

if(SKIAUI_BUILD_DEMOS AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_DEMOS requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_SKIA_UI_DESK AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_SKIA_UI_DESK requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_TEXT_FILE_DEMO AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_TEXT_FILE_DEMO requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_IMAGE_SCROLLER_DEMO requires SKIAUI_BUILD_WIN32_DX12.")
endif()

add_library(Skui STATIC
    src/skui/core/skui_layout.cpp
    src/skui/core/skui_parser.cpp
    src/skui/core/skui_runtime.cpp
    src/skui/core/skui_utils.cpp
    src/skui/render/skui_skia_renderer.cpp
)

if(SKIAUI_BUILD_WIN32)
    add_library(SkuiWin32 STATIC
        src/skui/platform/win32_event_adapter.cpp
    )
endif()

if(SKIAUI_BUILD_WIN32_DX12)
    add_library(SkuiWin32Dx12 STATIC
        src/skui/platform/win32_dx12_app.cpp
        src/d3d_context.cpp
        src/d3d_presenter.cpp
    )
endif()

if(SKIAUI_BUILD_DEMOS)
    add_executable(SkiaRelayDesk WIN32
        src/main.cpp
    )

    add_executable(SkiaLayerDesk WIN32
        src/layer_desk.cpp
        src/d3d_context.cpp
        src/d3d_presenter.cpp
    )
    add_executable(SkiaRelayDeskDemo WIN32
        src/skui_relay_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_SKIA_UI_DESK)
    add_executable(SkiaUiDesk WIN32
        src/skui_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_TEXT_FILE_DEMO)
    add_executable(SkiaTextFileDemo WIN32
        src/skui_text_file_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO)
    add_executable(SkiaImageScrollerDemo WIN32
        src/skui_image_scroller_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_TESTS)
    add_executable(SkuiInteractionTests
        tests/skui_interaction_tests.cpp
    )
endif()

target_include_directories(Skui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public>"
    "$<INSTALL_INTERFACE:include/skiaui>"
)

target_include_directories(Skui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/core"
)

target_link_libraries(Skui PUBLIC
    unofficial::skia::skia
    unofficial::skia::modules::svg
    yoga::yogacore
)

target_link_libraries(Skui PRIVATE
    lexbor::lexbor_static
)

target_compile_definitions(Skui PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(SKIAUI_BUILD_WIN32)
    target_include_directories(SkuiWin32 PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public>"
        "$<INSTALL_INTERFACE:include/skiaui>"
    )

    target_link_libraries(SkuiWin32 PUBLIC
        Skui
    )

    target_link_libraries(SkuiWin32 PRIVATE
        user32
        imm32
    )

    target_compile_definitions(SkuiWin32 PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

if(SKIAUI_BUILD_WIN32_DX12)
    target_include_directories(SkuiWin32Dx12 PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public>"
        "$<INSTALL_INTERFACE:include/skiaui>"
    )

    target_link_libraries(SkuiWin32Dx12 PUBLIC
        SkuiWin32
    )

    target_link_libraries(SkuiWin32Dx12 PRIVATE
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        d3dcompiler
        dbghelp
        ole32
        imm32
        shell32
        usp10
        windowscodecs
    )

    target_compile_definitions(SkuiWin32Dx12 PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

if(MSVC)
    target_compile_options(Skui PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    if(SKIAUI_BUILD_WIN32)
        target_compile_options(SkuiWin32 PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()
    if(SKIAUI_BUILD_WIN32_DX12)
        target_compile_options(SkuiWin32Dx12 PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()
endif()

if(SKIAUI_BUILD_DEMOS)
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
            d3dcompiler
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

    target_include_directories(SkiaRelayDeskDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaRelayDeskDemo PRIVATE
        SkuiWin32Dx12
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        d3dcompiler
        dbghelp
        ole32
        shell32
        usp10
        windowscodecs
    )

    target_compile_definitions(SkiaRelayDeskDemo PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaRelayDeskDemo PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()

    file(GLOB_RECURSE SKUI_RELAY_DEMO_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_relay_demo/*"
    )

    add_custom_target(SkiaRelayDeskDemoAssets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_relay_demo"
                "$<TARGET_FILE_DIR:SkiaRelayDeskDemo>/assets/skui_relay_demo"
        DEPENDS ${SKUI_RELAY_DEMO_ASSETS}
    )

    add_dependencies(SkiaRelayDeskDemo SkiaRelayDeskDemoAssets)
endif()

if(SKIAUI_BUILD_SKIA_UI_DESK)
    target_include_directories(SkiaUiDesk PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaUiDesk PRIVATE
        SkuiWin32Dx12
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        d3dcompiler
        dbghelp
        usp10
    )

    target_compile_definitions(SkiaUiDesk PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaUiDesk PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()

    add_custom_command(TARGET SkiaUiDesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_demo"
                "$<TARGET_FILE_DIR:SkiaUiDesk>/assets/skui_demo"
    )
endif()

if(SKIAUI_BUILD_TEXT_FILE_DEMO)
    target_include_directories(SkiaTextFileDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaTextFileDemo PRIVATE
        SkuiWin32Dx12
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        d3dcompiler
        dbghelp
        usp10
        comdlg32
    )

    target_compile_definitions(SkiaTextFileDemo PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaTextFileDemo PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()

    add_custom_command(TARGET SkiaTextFileDemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_text_file_demo"
                "$<TARGET_FILE_DIR:SkiaTextFileDemo>/assets/skui_text_file_demo"
    )
endif()

if(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO)
    target_include_directories(SkiaImageScrollerDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaImageScrollerDemo PRIVATE
        SkuiWin32Dx12
        user32
        gdi32
        dwmapi
        shcore
        d3d12
        dxgi
        dxguid
        d3dcompiler
        dbghelp
        usp10
    )

    target_compile_definitions(SkiaImageScrollerDemo PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaImageScrollerDemo PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()
endif()

if(SKIAUI_BUILD_TESTS)
    target_link_libraries(SkuiInteractionTests PRIVATE
        Skui
        usp10
    )

    if(SKIAUI_BUILD_WIN32)
        target_link_libraries(SkuiInteractionTests PRIVATE
            SkuiWin32
        )
    endif()

    if(MSVC)
        target_compile_options(SkuiInteractionTests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()

    if(BUILD_TESTING)
        add_test(NAME SkuiInteractionTests COMMAND SkuiInteractionTests)
    endif()
endif()

if(SKIATEST_USE_SKIA_D3D)
    if(SKIAUI_BUILD_WIN32_DX12)
        target_compile_definitions(SkuiWin32Dx12 PUBLIC SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_DEMOS)
        target_compile_definitions(SkiaLayerDesk PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_SKIA_UI_DESK)
        target_compile_definitions(SkiaUiDesk PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_TEXT_FILE_DEMO)
        target_compile_definitions(SkiaTextFileDemo PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO)
        target_compile_definitions(SkiaImageScrollerDemo PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
endif()

set(SKIAUI_INSTALL_TARGETS Skui)

set(SKIAUI_INSTALL_WIN32_TARGET OFF)
if(SKIAUI_BUILD_WIN32 AND SKIAUI_INSTALL_WIN32)
    set(SKIAUI_INSTALL_WIN32_TARGET ON)
endif()
if(SKIAUI_BUILD_WIN32_DX12 AND SKIAUI_INSTALL_WIN32_DX12)
    set(SKIAUI_INSTALL_WIN32_TARGET ON)
endif()
if(SKIAUI_INSTALL_WIN32_TARGET)
    list(APPEND SKIAUI_INSTALL_TARGETS SkuiWin32)
endif()

if(SKIAUI_BUILD_WIN32_DX12 AND SKIAUI_INSTALL_WIN32_DX12)
    list(APPEND SKIAUI_INSTALL_TARGETS SkuiWin32Dx12)
endif()

install(TARGETS ${SKIAUI_INSTALL_TARGETS}
    EXPORT SkiaUITargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

set(SKIAUI_PUBLIC_HEADERS
    src/skui/public/skui_dropdown.h
    src/skui/public/skui_runtime.h
    src/skui/public/skui_runtime_helpers.h
    src/skui/public/skui_virtual_table.h
    src/skui/public/skui_virtual_window.h
)

if(SKIAUI_INSTALL_WIN32_TARGET)
    list(APPEND SKIAUI_PUBLIC_HEADERS
        src/skui/public/skui_win32_event_adapter.h
    )
endif()

if(SKIAUI_BUILD_WIN32_DX12 AND SKIAUI_INSTALL_WIN32_DX12)
    list(APPEND SKIAUI_PUBLIC_HEADERS
        src/skui/public/skui_win32_app.h
        src/d3d_context.h
        src/d3d_presenter.h
        src/perf_trace.h
    )
endif()

install(FILES
    ${SKIAUI_PUBLIC_HEADERS}
    DESTINATION include/skiaui
)

if(SKIAUI_BUILD_SKIA_UI_DESK AND SKIAUI_INSTALL_SKIA_UI_DESK)
    install(TARGETS SkiaUiDesk
        RUNTIME DESTINATION tools/skiaui
    )

    install(DIRECTORY assets/skui_demo
        DESTINATION tools/skiaui/assets
    )
endif()

install(EXPORT SkiaUITargets
    NAMESPACE SkiaUI::
    DESTINATION share/skiaui
)

configure_package_config_file(
    cmake/SkiaUIConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfig.cmake"
    INSTALL_DESTINATION share/skiaui
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfigVersion.cmake"
    DESTINATION share/skiaui
)
