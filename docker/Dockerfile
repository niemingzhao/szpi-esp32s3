# 构建 / 开发镜像：ESP-IDF v6.1 + 本项目工具脚本要用的 Python 包。
#
# 用法（在仓库根目录）：
#   docker build -t szpi-os-builder docker
#   docker run --rm -it -v "${PWD}:/project" szpi-os-builder idf.py build    # PowerShell；cmd 用 %cd%
#
# 烧录要访问串口，Linux 下加 --device=/dev/ttyUSB0（Windows 用 WSL2 或直接在宿主机烧）。
FROM espressif/idf:v6.1

# ccache：配合 IDF_CCACHE_ENABLE 加速重复构建
RUN apt-get update \
    && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/*

# pillow 供 tools/gen_fw_*.py 与 tools/tests 使用，pre-commit 供本地钩子使用
RUN python -m pip install --no-cache-dir --disable-pip-version-check pillow pre-commit

ENV IDF_CCACHE_ENABLE=1
WORKDIR /project
