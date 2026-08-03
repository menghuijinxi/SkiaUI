vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pnggroup/libpng
    REF dcc3505c7bd0660876fc80110f8e3581900a186a
    SHA512 2241e11450d358add3234b5013eaa203e865d15155728da666733bcebf72927629acc055a698558fb15da22f9a9100db00fe5f5fbe7d523088f1009860e7192f
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DPNG_SHARED=OFF
        -DPNG_STATIC=ON
        -DPNG_TESTS=OFF
        -DSKIP_INSTALL_EXECUTABLES=ON
)
vcpkg_cmake_install()

# libpng 1.5 names its MSVC static target libpng15_static, while its
# pkg-config metadata requests libpng15. Normalize the installed name so Skia's
# pkg-config based dependency import resolves the actual archive.
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    foreach(config_dir IN ITEMS "" "debug/")
        set(static_library
            "${CURRENT_PACKAGES_DIR}/${config_dir}lib/libpng15_static.lib")
        if(EXISTS "${static_library}")
            file(RENAME "${static_library}"
                "${CURRENT_PACKAGES_DIR}/${config_dir}lib/libpng15.lib")
        endif()
    endforeach()
    foreach(pkgconfig_file IN ITEMS
        "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libpng.pc"
        "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libpng15.pc")
        if(EXISTS "${pkgconfig_file}")
            vcpkg_replace_string("${pkgconfig_file}" "-lpng15" "-llibpng15")
            vcpkg_replace_string("${pkgconfig_file}"
                "Libs.private: -lz -lm" "Requires.private: zlib")
        endif()
    endforeach()
endif()

vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/bin"
    "${CURRENT_PACKAGES_DIR}/debug/bin"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/lib/libpng"
    "${CURRENT_PACKAGES_DIR}/share/man"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
