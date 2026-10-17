# 本地源码开发与 FFmpeg n9.0.2

本机可以构建 C 服务和完整 Linux 镜像。Windows 下运行 Linux C 服务使用 WSL Ubuntu；Docker Desktop 也使用 Linux engine。之前冷构建遇到过 Ubuntu 软件源连接失败，不代表构建能力缺失。CI 产物是发布途径，也可供只改应用的构建复用。

## 修改 FFmpeg 后怎么编译

在 WSL 中：

```sh
cd /mnt/d/Code/ffmpeg_live
bash scripts/build-service.sh
./out/bin/naki-live
```

该入口默认调用 `build-ffmpeg.sh`，从本地子模块源码编译，再编译 C 服务；并不自动下载或使用 CI SDK。依赖源码内容改变时先清理对应组件的对象，避免旧 mtime 使 Make/CMake 复用旧代码；应用自身仍按通常方式增量构建。修改 configure 配方后会重新配置。原有异步/网络 API 均保留在 fork 中，可以继续修改。

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

## 本地 SDK 来源与复用检查

默认构建先对依赖源码及构建配方取内容摘要，成功安装后记录 schema 2 的 `install/share/naki/ffmpeg-build.json`：源码摘要、配方、工具版本、成功构建的组件内容键及安装头文件/库的 SHA256。编译期间源码改变、组件构建凭据缺失或失配都会拒绝封存。这里不读取 VVenC 的 Git 元数据，也不声称不同工具链能生成逐字相同的库。

`NAKI_SKIP_FFMPEG=1` 现在必须有匹配清单；安装产物变动、源码/配方改变、来源清单缺失都会明确拒绝。旧缓存需要先运行一次默认构建；不会自动下载或替换 SDK。只有两份依赖源码都缺省的容器应用阶段才按导入 SDK 清单验证其字节，不能拿半份源码绕过核对。

FFmpeg 配置键包括源码位置、configure/构建配方及编译环境；另以源码内容键决定对象是否可复用。每次依赖构建先把上次成功键读入当前进程，再在任何配置、清理、编译、安装前撤销磁盘上的统一缓存凭据及安装清单。组件只写 `.naki-object-key.pending`，两库封存检查全部成功后才原子提交构建根目录的 `.naki-object-keys.json`；旧的组件 `.naki-object-key` 不再授权复用。失败、封存拒绝或提交前被 SIGKILL 时不依赖退出 trap 保证失效，下次必须清理对象。单独运行 VVenC 脚本不会提交整个 SDK 的缓存凭据。

需要重新 configure 时，同样先撤销 `.naki-configure-key`，成功返回后才原子提交新配置键；失败或提交前中断不会留下可复用旧键。生成的 `config.h`、`ffbuild/config.mak`、`Makefile` 缺失也触发重新配置。配置成功后发生的编译失败可保留该完整配置键，对象复用仍受整个 SDK 提交凭据约束。

FFmpeg/VVenC 内容改变或无成功凭据时，只清理各自专用构建目录中的对象；代价是内容变化时整组件重编，而不是依赖源码 mtime。`tests/sdk_build_integration.py` 使用真实微型 DSO 及项目默认 build→seal→skip 入口，包含保留旧 mtime 的成功路径，以及部分对象已生成后的编译失败、SIGTERM/SIGKILL、封存失败再恢复旧源码路径。新增失败恢复用例在本机暂因 WSL `E_ACCESSDENIED` 未运行，见 [验证阻塞记录](validation-sdk-failure-recovery.md)。发行 SDK 的 `ffmpeg-key.sh` 另要求两份子模块的实际 HEAD 匹配父 gitlink，源码与配方都已提交；无法读 Git 元数据时明确失败，不猜测版本。先提交库、再更新父 gitlink 的流程保持不变。

快速探流使用 fork 的 `AVFORMAT_LIVE_VIDEO_PROBE_VERSION=1`，libavformat 微版本为 63.3.103。服务选择与探流一致的首条非附图 H.264/HEVC 视频；保留探测包，音频/数据并不保证初始化时完整。码率/帧率只按已知信息估计，VFR 不承诺短探流即可精确识别；字节/时间/输入中断预算仍然生效。NOBUFFER/NOPARSE 明确拒绝。专项入口 `python3 tests/probe_integration.py` 记录完整包账本、像素哈希、时间戳与探流耗时，实流连续性沿用 `tests/live_mux_integration.py`。

## 本次版本和 VVenC 边界

截至本次核实，最新稳定版为 n9.0.2；n9.1-dev 是开发 tag。n9.0.2 原生包含 libvvenc 封装，configure 要求 `libvvenc >= 1.6.1`。VVenC 官方说明 FFmpeg 从 7.1 起支持它。[FFmpeg 发布页](https://ffmpeg.org/download.html)、[VVenC 接入说明](https://github.com/fraunhoferhhi/vvenc/wiki/FFmpeg-Integration)。

服务现已接入 VVenC v1.14.0：独立 fork 子模块、`--enable-libvvenc`、10-bit filter、PTS 关联、排空与 VVC 输出验证；两个源码 pin 均进入 SDK 键，VVenC 单独归档 Build ID 符号。修改入口与验证见 [VVenC 接入](vvenc.md)。

选择 libvvenc 时 filter 输出 yuv420p10le，编码参数不再使用 x264 的 tune。VVenC 内部前瞻、帧重排、线程调度与首帧/I 帧等待计入新任务的测量，不沿用 x264 的 zerolatency 假设。[n9.0.2 libvvenc 源码](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/libvvenc.c)。
