cmake_minimum_required(VERSION 3.24)

project(SkiaRelayDesk VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# VS 2026 bundled CMake can decode the localized MSVC /showIncludes
# prefix twice. Ninja then cannot record header dependencies, allowing stale
# object files to survive incremental builds.
if(MSVC
    AND CMAKE_GENERATOR MATCHES "Ninja"
    AND CMAKE_CXX_CL_SHOWINCLUDES_PREFIX STREQUAL "娉ㄦ剰: 鍖呭惈鏂囦欢:  ")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")
endif()

include(CTest)
include(CMakePackageConfigHelpers)

find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)

option(SKIAUI_BUILD_DEMOS "Build SkiaUI demo applications" ON)
option(SKIAUI_BUILD_SKIA_UI_DESK "Build the SkiaUiDesk demo application" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_TEXT_FILE_DEMO "Build the text file rendering performance demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_IMAGE_SCROLLER_DEMO "Build the image scroller memory demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_DYNAMIC_DOM_DEMO "Build the dynamic DOM mutation demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_HTML_PREVIEWER "Build the standalone HTML preview window" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_BUILD_VIDEO_DEMO "Build the FFmpeg video playback demo" ${SKIAUI_BUILD_DEMOS})
option(SKIAUI_INSTALL_SKIA_UI_DESK "Install the SkiaUiDesk demo application" OFF)
option(SKIAUI_BUILD_TESTS "Build SkiaUI tests" ${BUILD_TESTING})
option(SKIAUI_BUILD_WIN32 "Build the Win32 SkUI platform adapter target" ON)
option(SKIAUI_INSTALL_WIN32 "Install the Win32 SkUI platform adapter target" ON)
option(SKIAUI_BUILD_WIN32_DX12 "Build the Win32/DX12 SkUI host target" ON)
option(SKIAUI_INSTALL_WIN32_DX12 "Install the Win32/DX12 SkUI host target" ON)
option(SKIAUI_ENABLE_FFMPEG_VIDEO "Build FFmpeg video and audio playback support" ON)
set(SKIAUI_FFMPEG_TEST_MEDIA "" CACHE FILEPATH
    "Optional media file used by the FFmpeg integration test")
option(SKIATEST_USE_SKIA_D3D "Render SkiaLayerDesk through Skia Ganesh D3D12 backend" ON)

if(SKIAUI_ENABLE_FFMPEG_VIDEO)
    find_package(FFMPEG REQUIRED)
    if(NOT TARGET FFMPEG::FFMPEG)
        add_library(FFMPEG::FFMPEG INTERFACE IMPORTED)
        target_include_directories(FFMPEG::FFMPEG INTERFACE
            ${FFMPEG_INCLUDE_DIRS}
        )
        target_link_libraries(FFMPEG::FFMPEG INTERFACE
            ${FFMPEG_LIBRARIES}
        )
    endif()
endif()

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

if(SKIAUI_BUILD_DYNAMIC_DOM_DEMO AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_DYNAMIC_DOM_DEMO requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_HTML_PREVIEWER AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_HTML_PREVIEWER requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_VIDEO_DEMO AND NOT SKIAUI_BUILD_WIN32_DX12)
    message(FATAL_ERROR "SKIAUI_BUILD_VIDEO_DEMO requires SKIAUI_BUILD_WIN32_DX12.")
endif()

if(SKIAUI_BUILD_VIDEO_DEMO AND NOT SKIAUI_ENABLE_FFMPEG_VIDEO)
    message(FATAL_ERROR "SKIAUI_BUILD_VIDEO_DEMO requires SKIAUI_ENABLE_FFMPEG_VIDEO.")
endif()

add_library(Skui STATIC
    src/skui/core/skui_layout.cpp
    src/skui/core/skui_media_controller.cpp
    src/skui/core/skui_parser.cpp
    src/skui/core/skui_runtime.cpp
    src/skui/core/skui_utils.cpp
    src/skui/render/skui_skia_renderer.cpp
)

if(SKIAUI_ENABLE_FFMPEG_VIDEO)
    add_library(SkuiFfmpeg STATIC
        src/skui/media/ffmpeg_decoder.cpp
        src/skui/media/ffmpeg_media_player.cpp
    )
endif()

if(SKIAUI_BUILD_WIN32)
    add_library(SkuiWin32 STATIC
        src/skui/platform/win32_event_adapter.cpp
    )
endif()

if(SKIAUI_BUILD_WIN32_DX12)
    add_library(SkuiWin32Dx12 STATIC
        src/skui/platform/frame_pacing_monitor.cpp
        src/skui/platform/win32_dx12_app.cpp
        src/d3d_context.cpp
        src/d3d_presenter.cpp
    )
    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        target_sources(SkuiWin32Dx12 PRIVATE
            src/skui/platform/wasapi_audio_output.cpp
        )
    endif()
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

if(SKIAUI_BUILD_DYNAMIC_DOM_DEMO)
    add_executable(SkiaDynamicDomDemo WIN32
        src/skui_dynamic_dom_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_HTML_PREVIEWER)
    add_executable(SkiaHtmlPreviewer WIN32
        src/skui_html_previewer/main.cpp
    )
endif()

if(SKIAUI_BUILD_VIDEO_DEMO)
    add_executable(SkiaVideoDemo WIN32
        src/skui_video_demo/main.cpp
    )
endif()

if(SKIAUI_BUILD_TESTS)
    add_executable(SkuiInteractionTests
        tests/skui_interaction_tests.cpp
    )
    add_executable(SkuiKeyboardEventTests
        tests/skui_keyboard_event_tests.cpp
    )
    add_executable(SkuiSelectableLinkTests
        tests/skui_selectable_link_tests.cpp
    )
    add_executable(SkuiMediaRuntimeTests
        tests/skui_media_runtime_tests.cpp
    )
    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        add_executable(SkuiFfmpegPlayerTests
            tests/skui_ffmpeg_player_tests.cpp
        )
    endif()
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

if(SKIAUI_ENABLE_FFMPEG_VIDEO)
    target_include_directories(SkuiFfmpeg PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public>"
        "$<INSTALL_INTERFACE:include/skiaui>"
    )
    target_link_libraries(SkuiFfmpeg PUBLIC
        Skui
    )
    target_link_libraries(SkuiFfmpeg PRIVATE
        FFMPEG::FFMPEG
    )
    target_compile_definitions(SkuiFfmpeg PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

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
        windowsapp
        imm32
        shell32
        usp10
        windowscodecs
    )

    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        target_link_libraries(SkuiWin32Dx12 PRIVATE
            SkuiFfmpeg
            avrt
        )
        target_compile_definitions(SkuiWin32Dx12 PRIVATE
            SKIAUI_HAS_FFMPEG_VIDEO=1
        )
    endif()

    target_compile_definitions(SkuiWin32Dx12 PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

if(MSVC)
    target_compile_options(Skui
        PUBLIC /utf-8 /Zc:__cplusplus
        PRIVATE /permissive-
    )
    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        target_compile_options(SkuiFfmpeg PRIVATE /permissive-)
    endif()
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

if(SKIAUI_BUILD_DYNAMIC_DOM_DEMO)
    file(GLOB_RECURSE SKUI_DYNAMIC_DOM_DEMO_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_dynamic_dom_demo/*"
    )

    add_custom_target(SkiaDynamicDomDemoAssets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_dynamic_dom_demo"
                "$<TARGET_FILE_DIR:SkiaDynamicDomDemo>/assets/skui_dynamic_dom_demo"
        DEPENDS ${SKUI_DYNAMIC_DOM_DEMO_ASSETS}
    )

    add_dependencies(SkiaDynamicDomDemo SkiaDynamicDomDemoAssets)

    target_include_directories(SkiaDynamicDomDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaDynamicDomDemo PRIVATE
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

    target_compile_definitions(SkiaDynamicDomDemo PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaDynamicDomDemo PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()
endif()

if(SKIAUI_BUILD_HTML_PREVIEWER)
    target_include_directories(SkiaHtmlPreviewer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaHtmlPreviewer PRIVATE
        SkuiWin32Dx12
        comdlg32
        shell32
    )

    target_compile_definitions(SkiaHtmlPreviewer PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaHtmlPreviewer PRIVATE
            /utf-8
            /permissive-
            /Zc:__cplusplus
        )
    endif()
endif()

if(SKIAUI_BUILD_VIDEO_DEMO)
    file(GLOB_RECURSE SKUI_VIDEO_DEMO_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_video_demo/*"
    )

    add_custom_target(SkiaVideoDemoAssets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/skui_video_demo"
                "$<TARGET_FILE_DIR:SkiaVideoDemo>/assets/skui_video_demo"
        DEPENDS ${SKUI_VIDEO_DEMO_ASSETS}
    )

    add_dependencies(SkiaVideoDemo SkiaVideoDemoAssets)

    target_include_directories(SkiaVideoDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
    )

    target_link_libraries(SkiaVideoDemo PRIVATE
        SkuiWin32Dx12
    )

    target_compile_definitions(SkiaVideoDemo PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(SkiaVideoDemo PRIVATE
            /utf-8
            /permissive-
            /Zc:__cplusplus
        )
    endif()
endif()

if(SKIAUI_BUILD_TESTS)
    target_link_libraries(SkuiInteractionTests PRIVATE
        Skui
        usp10
    )
    target_link_libraries(SkuiKeyboardEventTests PRIVATE
        Skui
        usp10
    )
    target_link_libraries(SkuiSelectableLinkTests PRIVATE
        Skui
        usp10
    )
    target_link_libraries(SkuiMediaRuntimeTests PRIVATE
        Skui
        usp10
    )
    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        target_link_libraries(SkuiFfmpegPlayerTests PRIVATE
            SkuiFfmpeg
        )
    endif()

    if(SKIAUI_BUILD_WIN32)
        target_link_libraries(SkuiInteractionTests PRIVATE
            SkuiWin32
        )
    endif()

    if(MSVC)
        target_compile_options(SkuiInteractionTests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
        target_compile_options(SkuiKeyboardEventTests PRIVATE
            /utf-8
            /permissive-
            /Zc:__cplusplus
        )
        target_compile_options(SkuiSelectableLinkTests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
        target_compile_options(SkuiMediaRuntimeTests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
        if(SKIAUI_ENABLE_FFMPEG_VIDEO)
            target_compile_options(SkuiFfmpegPlayerTests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
        endif()
    endif()

    if(BUILD_TESTING)
        add_test(NAME SkuiInteractionTests COMMAND SkuiInteractionTests)
        add_test(NAME SkuiKeyboardEventTests COMMAND SkuiKeyboardEventTests)
        add_test(NAME SkuiSelectableLinkTests COMMAND SkuiSelectableLinkTests)
        add_test(NAME SkuiMediaRuntimeTests COMMAND SkuiMediaRuntimeTests)
        if(SKIAUI_ENABLE_FFMPEG_VIDEO)
            add_test(NAME SkuiFfmpegPlayerTests
                COMMAND SkuiFfmpegPlayerTests
                        "${CMAKE_SOURCE_DIR}/tests/fixtures/transparent-vp9.webm"
                        --expect-vp9-alpha)
        endif()
        if(SKIAUI_ENABLE_FFMPEG_VIDEO AND SKIAUI_FFMPEG_TEST_MEDIA)
            add_test(NAME SkuiFfmpegExternalMediaTests
                COMMAND SkuiFfmpegPlayerTests "${SKIAUI_FFMPEG_TEST_MEDIA}")
        endif()
        set_tests_properties(
            SkuiInteractionTests
            SkuiKeyboardEventTests
            SkuiSelectableLinkTests
            SkuiMediaRuntimeTests
            PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
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
    if(SKIAUI_BUILD_DYNAMIC_DOM_DEMO)
        target_compile_definitions(SkiaDynamicDomDemo PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_HTML_PREVIEWER)
        target_compile_definitions(SkiaHtmlPreviewer PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
    if(SKIAUI_BUILD_VIDEO_DEMO)
        target_compile_definitions(SkiaVideoDemo PRIVATE SKIATEST_USE_SKIA_D3D=1)
    endif()
endif()

set(SKIAUI_INSTALL_TARGETS Skui)

if(SKIAUI_ENABLE_FFMPEG_VIDEO)
    list(APPEND SKIAUI_INSTALL_TARGETS SkuiFfmpeg)
endif()

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
    src/skui/public/skui_media.h
    src/skui/public/skui_runtime.h
    src/skui/public/skui_runtime_helpers.h
    src/skui/public/skui_virtual_table.h
    src/skui/public/skui_virtual_window.h
)

if(SKIAUI_ENABLE_FFMPEG_VIDEO)
    list(APPEND SKIAUI_PUBLIC_HEADERS
        src/skui/public/skui_ffmpeg.h
    )
endif()

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
    if(SKIAUI_ENABLE_FFMPEG_VIDEO)
        list(APPEND SKIAUI_PUBLIC_HEADERS
            src/skui/public/skui_win32_audio.h
        )
    endif()
endif()

install(FILES
    ${SKIAUI_PUBLIC_HEADERS}
    DESTINATION include/skiaui
)

install(FILES README.md
    DESTINATION share/skiaui
)

install(DIRECTORY docs/
    DESTINATION share/skiaui/docs
    FILES_MATCHING PATTERN "*.md"
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
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SkiaUIConfigVersion.cmake"
    DESTINATION share/skiaui
)
