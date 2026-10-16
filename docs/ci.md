# Linux 构建、镜像和符号归档

目前只构建、测试 **Linux amd64**。Windows 可通过 WSL 使用构建和解析脚本；尚未验证 ARM64 镜像。

## 两条构建流程

- **FFmpeg dependency**：独立、可手动执行的可复用 workflow。服务 CI 先调用它查找 SDK，只有缺少匹配产物时才编译。键包含固定的 FFmpeg/VVenC gitlink、Ubuntu/架构标识、`Dockerfile.ffmpeg`、两者构建脚本和符号导出脚本的哈希。
- **Service CI**：main push、PR、手动触发。下载确切 SDK，校验 SHA256，构建 C 服务、运行镜像、导出独立符号，并运行完整链路、观测、进程接管、轻量崩溃、离线解析和 Compose 镜像测试。应用源码变化不会重新编译 FFmpeg。

FFmpeg/VVenC SDK 保存为公共仓库的 prerelease `ffmpeg-linux-amd64-<recipe-key>`。SDK 的安装前缀固定为 `/opt/naki-build/install`，包括共享库、头文件、pkg-config、FFmpeg/VVenC 独立调试符号、configure/CMake 配置和构建包版本。应用的 Docker build 使用 `FFMPEG_STAGE=ffmpeg-prebuilt`，不会执行本地依赖构建阶段。没有匹配 SDK 的 PR 会要求维护者在可信分支手动运行依赖 workflow；不从 PR 发布依赖。

依赖 release 发布后复用原文件。键标识源码和构建配方；Ubuntu 镜像标签和 apt 仓库本身会更新，因此**不声称位级可重现**。更新工具链时应修改依赖 Dockerfile（建议固定所需镜像 digest/包版本），产生新键。`packages.txt` 记录实际使用的包版本；排查时以 ELF Build ID 为准。

## 每次 main 构建的发布材料

测试通过后，每轮 main 构建创建独立 prerelease，包含完整提交号、Actions run ID 和 attempt，因此同一提交的重新构建也不会覆盖之前的符号。

| 文件 | 内容 |
| --- | --- |
| `image-linux-amd64.tar.gz` | 测试过的运行镜像，`docker load` 导入 |
| `symbols-linux-amd64.tar.gz` | `app/`、`ffmpeg/`、`vvenc/` 各自的 `.build-id/xx/rest.debug`、manifest 和构建配置 |
| `image.json` | 镜像 ID、架构、源码与 FFmpeg 依赖键标签 |
| `source-revision.txt`、`ffmpeg-dependency-key.txt` | 源码与依赖索引 |
| `symbolize-crash.py` | 配套离线解析入口 |
| `SHA256SUMS` | 镜像包、符号包 SHA256 |

镜像与符号分别打包，运行镜像的应用 ELF 剥离 DWARF，仍保留 Build ID 和 unwind 信息。符号从同一构建的原始 ELF 导出；FFmpeg 使用构建目录中尚未剥离的库。manifest 记录每个模块的 Build ID、原 ELF/独立符号 SHA256、源码提交、构建工具和依赖键。

Actions 临时产物保留 90 天，测试证据保留 14 天；main 的 release 资产没有这两种自动到期策略。长期部署应另外备份部署过的镜像/符号/源码和依赖 release，并遵循自己的保留策略。删除 GitHub release 仍会丢失材料。

首版使用可下载镜像包，不依赖容器仓库账号或包可见性。导入运行：

```sh
gh run list --workflow service.yml
gh release download <build-release-tag> -D release
(cd release && sha256sum -c SHA256SUMS)
docker load -i release/image-linux-amd64.tar.gz
docker tag naki-live-transcode:<source-revision> naki-live-transcode:local
docker compose up -d --no-build transcode
```

本地仍支持 `docker compose up -d --build transcode`，会在本地构建 FFmpeg。若想复用 SDK：

```sh
mkdir -p out
gh release download <ffmpeg-sdk-release-tag> -p 'ffmpeg-sdk.tar.gz*' -D out
sha256sum -c out/ffmpeg-sdk.tar.gz.sha256
docker build --build-arg FFMPEG_STAGE=ffmpeg-prebuilt -t naki-live-transcode:local .
docker build --build-arg FFMPEG_STAGE=ffmpeg-prebuilt --target symbols \
  --output type=local,dest=out/symbols .
```

## 当前 FFmpeg 裁剪范围

仓库引用完整、固定版本的 FFmpeg fork，保留异步解码与 TCP/UDP 观测修改；服务构建通过 `--disable-everything` 按需启用组件，不是重新实现 FFmpeg：

当前基于 n9.0.2，历史目录名暂时保留。迁移和本地源码开发见 [FFmpeg 开发](ffmpeg-development.md)。已启用独立 pin 的 VVenC v1.14.0 和 libvvenc 封装，见 [VVenC 接入](vvenc.md)。SDK 键同时包含 VVenC gitlink 与构建脚本；VVenC 改动会触发依赖重建，单纯应用改动继续复用 SDK。

- H.264/HEVC/VVC、AAC/MP3 decoder/parser，libx264/libvvenc encoder；GPL 构建。VVC decoder 用于独立验收输出，服务视频入口仍只接受 H.264/HEVC；音频 decoder 只做有界参数探测与测试，不是音频转码管线。
- mov/mpegts/h264/hevc/flv/rtsp/aac/mp3 demuxer；mpegts/mp4/flv muxer，以及自动选择的 ADTS/LATM 依赖；aac_adtstoasc/extract_extradata BSF。
- file/pipe/udp/tcp/rtp/http/rtmp/libsrt protocol，network/pthreads。SRT 链接发行版 libsrt-openssl；构建安装 libsrt-openssl-dev 和 libssl-dev，运行层安装 libsrt1.5-openssl。
- buffer/buffersink/null/scale/format filter，以及 FFmpeg 自动选择的必需依赖。
- 无自动探测、硬件加速、音频转码链路；协议/封装被编译进去不等于已完成现网兼容性验证。

完整启用清单以 SDK 中的 `config.h`、`config.mak` 为准。发行版提供的 x264、libc、libevent、libunwind 等库的 DWARF **不包含**在自有符号包中；这些模块需按报告中的 Build ID 另取匹配的发行版 debug 包，不能用其他版本代替。

直播测试使用本地 RTMP/SRT 监听器与真实协议连接，不依赖外部媒体服务器；验证器持续发现新流并独立解码，而不是只统计探流时已存在的轨道。两条晚 AAC 音轨、停顿恢复、MP3/EOF 和交织硬上限均在 Service CI 中运行；证据保存到 `out/live-mux-*`。

参考：[GitHub workflow 产物下载](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/download-workflow-artifacts?tool=cli)、[Docker 镜像导出](https://docs.docker.com/build/ci/github-actions/export-docker/)。
