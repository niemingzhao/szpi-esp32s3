# ESP-IDF 顶层构建脚本
# 语句顺序固定：cmake_minimum_required → EXTRA_COMPONENT_DIRS → include(project.cmake) → project()
cmake_minimum_required(VERSION 3.22)

# 五个目录各注册为一个组件，对应五层架构
set(EXTRA_COMPONENT_DIRS
    main/drivers main/peripherals main/services main/framework main/apps)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(szpi-esp32s3)

# 第三方组件编译告警降级
# 只处理组件管理器安装的组件（名形如 namespace__name），本项目各层保持严格编译：
# IDF 对各组件都用 -Wall -Werror -Wextra，这些诊断在 GCC 14/15 会把第三方旧 C 代码判错。
set(_managed_downgrade
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
    -Wno-error=return-mismatch
    -Wno-error=unterminated-string-initialization
    -Wno-error=header-guard)

idf_build_get_property(_build_components BUILD_COMPONENTS)
foreach(_comp ${_build_components})
    if(_comp MATCHES "__")
        idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
        if(TARGET ${_lib})
            # 无源文件的组件注册为 INTERFACE 库，不编译代码
            get_target_property(_lib_type ${_lib} TYPE)
            if(NOT _lib_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_lib} PRIVATE ${_managed_downgrade})
            endif()
        endif()
    endif()
endforeach()

# esp_lvgl_port 的源文件在它自建的 lvgl_port_lib 里编译，不在组件库中
if(TARGET lvgl_port_lib)
    target_compile_options(lvgl_port_lib PRIVATE ${_managed_downgrade})
endif()
