# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS drivers peripherals services framework apps)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(szpi-esp32s3)

# ---- 第三方（托管）组件与 GCC 14 的兼容处理 ----
# 工具链是 GCC 14，它把 -Wincompatible-pointer-types / -Wint-conversion /
# -Wimplicit-function-declaration / -Wimplicit-int / -Wreturn-mismatch 从警告默认升级成错误，
# 托管组件里的旧 C 代码会因此编译失败（如 LVGL 8.3 自带的 lodepng：uint32_t* 传给 unsigned*）。
# IDF 5.4 的 CONFIG_COMPILER_DISABLE_GCC14_WARNINGS 只加 -Wno-calloc-transposed-args，覆盖不到，
# 所以在这里只对托管组件（名字形如 namespace__name）降级这几项。
# 本项目自己的 drivers / peripherals / services / framework / apps 不加，保持严格编译。
idf_build_get_property(_build_components BUILD_COMPONENTS)
foreach(_comp ${_build_components})
    if(_comp MATCHES "__")
        idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
        if(TARGET ${_lib})
            target_compile_options(${_lib} PRIVATE
                -Wno-error=incompatible-pointer-types
                -Wno-error=int-conversion
                -Wno-error=implicit-function-declaration
                -Wno-error=implicit-int
                -Wno-error=return-mismatch)
        endif()
    endif()
endforeach()
