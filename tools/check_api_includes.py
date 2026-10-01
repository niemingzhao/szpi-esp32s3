#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：函数/宏的声明头是否可见（避免 implicit declaration / undeclared）。

覆盖两类：

1. 跨层调用：调用了别的层的函数（drv_ / periph_ / svc_ / fw_ / app_ 前缀），
   但声明它的头文件不在该 .c 的可见 include 里。
   （踩过：periph_camera.c 缺 drv_common.h）
2. 常用 IDF / 标准库：用了 ESP_LOGx、esp_timer_*、malloc、snprintf 等，
   却没 include 对应的头文件。
   （踩过：svc_sysinfo.c 缺 esp_log.h —— GCC 14 下 implicit declaration 是错误）

"可见 include" = 直接 include 的头 + 这些头再 include 的头（一层"聚合头"传递，
例如 periph_common.h → drv_common.h、svc_common.h → svc_bt.h）。

用法：
    python tools/check_api_includes.py
退出码：0 = 通过，1 = 有缺失（列出文件与缺失项）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LAYERS = ('main',)
SKIP_DIRS = ('managed_components', 'build', '.git', 'tools', 'docs')

RET = (r'(?:esp_err_t|void|bool|int|char|unsigned|size_t|float|uint8_t|uint16_t|uint32_t'
       r'|int8_t|int16_t|int32_t|const char \*|lv_obj_t \*|lv_color_t|esp_gatt_if_t)')
DECL = re.compile(r'(?m)^\s*(?:extern\s+)?' + RET + r'(?:\s*\*)?\s+([a-z_][a-z0-9_]*)\s*\([^;{]*\)\s*;')
INC = re.compile(r'#include\s+[<"]([^>"]+)[>"]')
NS = re.compile(r'^(drv|periph|svc|fw|app)_')

# 我们约定"用到就直接 include"的头（不依赖传递包含）-> 检查是否有直接 include。
# 只收低噪声的：这些在项目里一直都是显式包含的；像 esp_err.h / esp_event.h 那种
# 常被 IDF 头传递带入的就不列，避免满屏误报。
NEEDS_DIRECT = [
    (r'\bESP_LOG[EWID]\s*\(', 'esp_log.h'),
    (r'\besp_timer_', 'esp_timer.h'),
    (r'\bheap_caps_', 'esp_heap_caps.h'),
    (r'\besp_chip_info\s*\(', 'esp_chip_info.h'),
    (r'\bsnprintf\s*\(|\bsscanf\s*\(', 'stdio.h'),
    (r'\bmalloc\s*\(|\bcalloc\s*\(|\bfree\s*\(|\bstrdup\s*\(|\babort\s*\(', 'stdlib.h'),
    (r'\bmemcpy\s*\(|\bmemset\s*\(|\bmemcmp\s*\(|\bmemmove\s*\(|\bstrlen\s*\(|\bstrcmp\s*\(|\bstrncmp\s*\('
     r'|\bstrncpy\s*\(|\bstrlcpy\s*\(|\bstrchr\s*\(|\bstrstr\s*\(', 'string.h'),
    (r'\bsqrtf?\s*\(|\batan2f?\s*\(|\bfabsf?\s*\(|\bsinf\s*\(|\bcosf\s*\(', 'math.h'),
    (r'\bvTaskDelay\s*\(|\bxTaskCreate\w*\s*\(|\bxTaskDelete\s*\(|\bpdMS_TO_TICKS\s*\('
     r'|\buxTaskGetSystemState\s*\(|\bpcTaskGetName\s*\(|\btskNO_AFFINITY\b|\bTaskStatus_t\b|\bStackType_t\b',
     'freertos/task.h'),
    (r'\bxSemaphore', 'freertos/semphr.h'),
    (r'\bportMAX_DELAY\b|\bpdTRUE\b|\bpdPASS\b', 'freertos/FreeRTOS.h'),
]


def is_source(path):
    return path.endswith(('.c', '.h'))


def walk_layers():
    for layer in LAYERS:
        base = os.path.join(ROOT, layer)
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in files:
                if is_source(f):
                    yield os.path.join(root, f)


def main():
    hdr_of_decl = {}
    direct_includes = {}

    for p in walk_layers():
        with open(p, 'r', encoding='utf-8', errors='ignore') as fh:
            text = fh.read()
        direct_includes[p] = set(INC.findall(text))
        if p.endswith('.h'):
            name = os.path.basename(p)
            for fn in DECL.findall(text):
                hdr_of_decl.setdefault(fn, []).append(name)

    findings = []
    for p in walk_layers():
        if not p.endswith('.c'):
            continue
        with open(p, 'r', encoding='utf-8', errors='ignore') as fh:
            text = fh.read()

        visible = set(direct_includes.get(p, set()))
        for inc in list(visible):                       # 聚合头一层传递
            for q, incs in direct_includes.items():
                if os.path.basename(q) == inc:
                    visible |= incs
                    break
        visible_base = set(os.path.basename(v) for v in visible)

        rel = os.path.relpath(p, ROOT).replace('\\', '/')

        # 1. 跨层调用
        for fn, headers in hdr_of_decl.items():
            if not NS.match(fn):
                continue
            if not re.search(r'(?<![a-zA-Z_])' + re.escape(fn) + r'\s*\(', text):
                continue
            if any(h in visible for h in headers):
                continue
            findings.append((rel, '%s()' % fn, '跨层函数，声明在 %s' % headers[0]))

        # 2. 约定"用到就显式 include"的头
        for rx, hdr in NEEDS_DIRECT:
            if not re.search(rx, text):
                continue
            if hdr in direct_includes.get(p, set()):
                continue
            findings.append((rel, re.search(rx, text).group(0).strip(), '需要直接 #include "%s"' % hdr))

    if not findings:
        print('OK: 用到的函数/宏都能看到声明头')
        return 0

    print('发现 %d 处声明头不可见（编译会报 implicit declaration / undeclared）：' % len(findings))
    for path, what, why in sorted(findings):
        print('  %s: %s  ->  %s' % (path, what, why))
    print('\n补齐对应 include（聚合头如 drv_common.h / periph_common.h / svc_common.h 也可）。')
    return 1


if __name__ == '__main__':
    sys.exit(main())
