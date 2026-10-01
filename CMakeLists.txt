# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    main/drivers main/peripherals main/services main/framework main/apps)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(szpi-esp32s3)

# ---- 第三方（托管）组件与 GCC 14 的兼容处理 ----
# 工具链是 GCC 14，它把 -Wincompatible-pointer-types / -Wint-conversion /
# -Wimplicit-function-declaration / -Wimplicit-int / -Wreturn-mismatch 从警告默认升级成错误，
# 托管组件里的旧 C 代码会因此编译失败（如图像解码库把 uint32_t* 传给 unsigned*）。
# CONFIG_COMPILER_DISABLE_GCC14_WARNINGS 只加 -Wno-calloc-transposed-args，覆盖不到，
# 所以在这里只对托管组件（名字形如 namespace__name）降级这几项。
# 本项目自己的 main/drivers|peripherals|services|framework|apps 不加，保持严格编译。
set(_gcc14_downgrade
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
    -Wno-error=return-mismatch)

idf_build_get_property(_build_components BUILD_COMPONENTS)
foreach(_comp ${_build_components})
    if(_comp MATCHES "__")
        idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
        if(TARGET ${_lib})
            # 没有源文件的托管组件（如 esp_lvgl_port）注册出来是 INTERFACE 库，
            # 对它只能用 INTERFACE 关键字，而它本身不编译任何代码，跳过即可。
            get_target_property(_lib_type ${_lib} TYPE)
            if(NOT _lib_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_lib} PRIVATE ${_gcc14_downgrade})
            endif()
        endif()
    endif()
endforeach()

# esp_lvgl_port 的源文件在它自己创建的 lvgl_port_lib 里编译（不在组件库里），同样降级。
if(TARGET lvgl_port_lib)
    target_compile_options(lvgl_port_lib PRIVATE ${_gcc14_downgrade})
endif()
