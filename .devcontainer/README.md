# 开发容器

给 VS Code 的 Dev Containers 扩展用：打开仓库根目录，选择「Reopen in Container」。

- 镜像就是官方 `espressif/idf:v6.1`（与 `docker/Dockerfile` 同源，不再单独维护一份
  构建文件），创建后由 `postCreateCommand` 装 pillow 与 pre-commit。
- 容器里已打开 ccache（`containerEnv` 里的 `IDF_CCACHE_ENABLE=1`），编译产物写在挂进
  容器的 `/project`，与宿主机共享。
- 容器里看不到宿主机串口，烧录建议在宿主机做；镜像构建、串口访问等宿主机相关的事
  见 `docker/README.md`。
