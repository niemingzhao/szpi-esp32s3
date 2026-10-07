# releases

发布用的合并固件放在这里：`szpi-esp32s3.bin`（外加这份说明），随版本一起提交。

- **它是什么**：`idf.py merge-bin` 把 bootloader、分区表与应用合并后的单一镜像，
  烧录时选它、地址填 `0x0` 即可（串口烧录工具、`esptool`、Flash Download Tool 都一样）。
- **谁写的**：`.github/workflows/release.yml` 发布时编译、合并并提交，同时作为 GitHub
  Release 的资产上传；`.github/workflows/ci.yml` 的 `refresh` job 在每次 main 推送构建成功
  后也会把它提交回来，让这里始终是最新一次构建。两者都只在 main 上写，且共用一个并发组。
- **不要手改**：它是构建产物，手改会在下次构建时被覆盖。要更新就等一次构建或发布。

当前固件版本见 `main/services/include/svc_identity.h` 的 `SZPI_OS_VERSION`。
