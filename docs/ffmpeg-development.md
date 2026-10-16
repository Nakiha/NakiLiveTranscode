# 本地源码开发与 FFmpeg n9.0.2

本机可以构建 C 服务和完整 Linux 镜像。Windows 下运行 Linux C 服务使用 WSL Ubuntu；Docker Desktop 也使用 Linux engine。之前冷构建遇到过 Ubuntu 软件源连接失败，不代表构建能力缺失。CI 产物是发布途径，也可供只改应用的构建复用。

## 修改 FFmpeg 后怎么编译

在 WSL 中：

```sh
cd /mnt/d/Code/ffmpeg_live
bash scripts/build-service.sh
./out/bin/naki-live
```

该入口默认调用 `build-ffmpeg.sh`，从本地子模块源码编译，再编译 C 服务；并不自动下载或使用 CI SDK。更改 `.c/.h` 后 Make 会增量构建，修改 configure 配方后会重新配置。原有异步/网络 API 均保留在 fork 中，可以继续修改。

默认缓存是 `${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9`，和原 7.0.3 的缓存分开，避免混用不同 ABI 的库。可设置 `NAKI_BUILD_ROOT` 选择独立前缀；Docker 内仍固定 `/opt/naki-build`。更改编译器、主版本或需要完全独立的实验时，选择新的构建根目录。输出程序在 `out/bin`，冻结媒体/历史报告不覆盖。

完整本地镜像构建仍为：

```sh
docker compose build transcode
docker compose up -d --no-build transcode
```

默认 `FFMPEG_STAGE=ffmpeg-local`，会从源码构建 FFmpeg。只有显式指定 `FFMPEG_STAGE=ffmpeg-prebuilt` 才使用校验过的 SDK 压缩包。`NAKI_SKIP_FFMPEG=1` 同样是显式跳过，适用于已安装匹配库、只重编应用的情况；修改库时使用默认入口。

## 将 FFmpeg 修改交付给 CI

FFmpeg 仍保留独立 Git 历史，不能只提交父仓库：

1. 在子模块内提交修改并推送到 `Nakiha/FFmpeg` 的当前 live 分支。
2. 在父仓库提交新的子模块 gitlink，并推送父仓库。
3. CI 的依赖键包含 FFmpeg gitlink 和构建配方。新的库提交触发新 SDK、独立符号，再构建和验证服务镜像；只改应用则复用 SDK。

`tests/decoder_integration.py` 会生成短 H.264/HEVC 样本，编译并运行 fork 的探针，验证 B 帧像素/元数据一致性、前台 gate、关闭/flush、容量 1 重放、多 owner 预算与默认预算生命周期。它不依赖本机冻结媒体，已纳入 CI。大规格、10-bit、变分辨率及长样本回归仍使用子模块专项脚本和新的结果目录；旧性能数据不代表升级后的性能。

## 本次版本和 VVenC 边界

截至本次核实，最新稳定版为 n9.0.2；n9.1-dev 是开发 tag。n9.0.2 原生包含 libvvenc 封装，configure 要求 `libvvenc >= 1.6.1`。VVenC 官方说明 FFmpeg 从 7.1 起支持它。[FFmpeg 发布页](https://ffmpeg.org/download.html)、[VVenC 接入说明](https://github.com/fraunhoferhhi/vvenc/wiki/FFmpeg-Integration)。

服务现已接入 VVenC v1.14.0：独立 fork 子模块、`--enable-libvvenc`、10-bit filter、PTS 关联、排空与 VVC 输出验证；两个源码 pin 均进入 SDK 键，VVenC 单独归档 Build ID 符号。修改入口与验证见 [VVenC 接入](vvenc.md)。

选择 libvvenc 时 filter 输出 yuv420p10le，编码参数不再使用 x264 的 tune。VVenC 内部前瞻、帧重排、线程调度与首帧/I 帧等待计入新任务的测量，不沿用 x264 的 zerolatency 假设。[n9.0.2 libvvenc 源码](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/libvvenc.c)。
