# ESP32NFS

**面向 ESP32 和 Raspberry Pi Pico W 的 NFSv3 文件共享库。** 通过 Wi-Fi 共享 SD 卡、内部 Flash、RAM 和固件资源，使用统一接口管理文件系统与分区。

[English](README.md) · [使用指南](docs/getting-started.md) · [存储接入](docs/storage.md) · [API](docs/api.md)

## 功能

- 通过 TCP 提供 NFSv3 和 MOUNT v3 服务，支持 Linux、macOS 客户端。
- 支持中文与 UTF-8 名称，使用文件系统原生大小写规则统一长文件名和短别名身份。
- 将多个设备、分区和文件系统映射为独立共享目录。
- 接入 LittleFS、FAT32、exFAT，以及 RAM 内存文件和 ROM 固件资源。
- 提供文件读写、目录遍历、随机访问、缓冲写入和同步操作。
- 通过扇区事务日志完成 FAT/exFAT 原子覆盖重命名与 EXCLUSIVE CREATE。
- 使用标准文件系统、块设备和传输接口接入 USB、SPI 与虚拟存储驱动。
- 配置传输块大小、带宽、连接数量和任务调度。

## 安装

1. 在 Arduino 开发板管理器中安装 **esp32 by Espressif Systems**。
2. 在库管理器中安装 **SdFat 2.3.0**。
3. 从 [Releases](https://github.com/cccat6/ESP32NFS/releases/latest) 下载 **ESP32NFS-1.3.0.zip**。
4. 在 Arduino IDE 中选择 **项目 → 加载库 → 添加 .ZIP 库**，导入压缩包。
5. 打开 **文件 → 示例 → ESP32NFS → LittleFSShare**，填写 Wi-Fi 信息后编译上传。

首次初始化空白 LittleFS 分区时，将示例中的 `FormatOnMountFailure` 设为 `true`；后续启动设为 `false`。串口会输出共享地址。

## 挂载

Linux：

```sh
sudo mkdir -p /mnt/esp32
sudo mount -t nfs -o vers=3,proto=tcp,port=2049,mountport=2049,nolock ESP32_IP:/ /mnt/esp32
```

macOS：

```sh
mkdir -p ~/mnt/esp32
mount_nfs -o vers=3,tcp,port=2049,mountport=2049,nolocks ESP32_IP:/ ~/mnt/esp32
```

## 存储映射

```cpp
ExportMap exports;
exports.add("/sd", sdFilesystem);
exports.add("/flash", flashFilesystem);
exports.add("/ram", ramFilesystem);
exports.add("/rom", romFilesystem, true);
esp32_nfs::Server nfs(exports);
```

每个共享路径对应自己的存储后端。客户端可以分别挂载 `/sd`、`/flash`、`/ram` 和 `/rom`。RAM 保存运行期间的数据；ROM 以只读方式提供编译进固件的文件；LittleFS 和 SD 卡保存持久化文件。

## 平台

CI 覆盖 ESP32、S2、S3、C2、C3、C5、C6、C61、H2、P4，以及 Pico W 和 Pico 2 W。[平台文档](docs/platforms.md)列出了各型号的构建方式和示例。

## 示例与文档

| 示例 | 用途 |
|---|---|
| [LittleFSShare](examples/LittleFSShare) | 共享内部 Flash 文件 |
| [SdCardShare](examples/SdCardShare) | 共享 FAT32 / exFAT SD 卡 |
| [MappedVolumes](examples/MappedVolumes) | 同时映射两个 SD 分区、RAM 和 ROM |
| [MemoryShares](examples/MemoryShares) | 提供内存文件和固件资源 |
| [PicoWShare](examples/PicoWShare) | Pico Wi-Fi 共享 RAM、ROM 和 LittleFS 资源 |
| [PicoSdCardShare](examples/PicoSdCardShare) | Pico Wi-Fi 共享 FAT32 / exFAT SD 卡 |

完整文档包含[使用指南](docs/getting-started.md)、[存储与元数据](docs/storage.md)、[驱动接口](docs/driver-interface.md)、[API](docs/api.md)、[性能配置](docs/performance.md)和[自动发布流程](docs/releasing.md)。

## 许可证

项目采用 [MIT License](LICENSE)。依赖说明见 [NOTICE.md](NOTICE.md)。

[名称与文件身份](docs/names-and-identities.md)介绍 UTF-8 配置、规范路径 API 和文件租约接入。
