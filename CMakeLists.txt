cmake_minimum_required(VERSION 3.24)

project(SkiaRelayDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)

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
