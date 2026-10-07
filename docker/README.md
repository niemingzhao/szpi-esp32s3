# Docker 与 devcontainer

两个用途不同的容器配置，都基于官方镜像 `espressif/idf:v6.1`：

- `docker/Dockerfile`：固件构建镜像，额外装了 ccache 与 Python 的 pillow / pre-commit，
  供本地或 CI 编译、跑工具脚本。
- `.devcontainer/devcontainer.json`：VS Code 开发容器，直接用官方镜像 + 创建后安装依赖。

## 构建镜像

```powershell
docker build -t szpi-os-builder docker
```

## 用容器编译

在仓库根目录执行（把当前目录挂进 `/project`）：

```powershell
docker run --rm -it -v "${PWD}:/project" szpi-os-builder idf.py build
```

首次进去要先设目标：`idf.py set-target esp32s3`。ccache 已打开
（`IDF_CCACHE_ENABLE=1`），重复构建会快很多。

## 烧录与串口

容器里看不到宿主机串口。要烧录有两种做法：

- 直接在宿主机（或 WSL2）用 ESP-IDF 环境烧，只把容器当编译环境；
- Linux 下给 `docker run` 加 `--device=/dev/ttyUSB0`（Windows 的 COM 口不支持直接映射）。

## devcontainer

用 VS Code 的 Dev Containers 扩展打开仓库根目录，选择「Reopen in Container」即可。
开发容器与构建镜像基于同一个官方镜像，创建后会自动装 pillow 与 pre-commit。
