# NakiLiveTranscode

实验性的 CPU 直播转码项目，起点为 FFmpeg 7.0.3 的异步提前出帧和自动并行度调度实验。

**当前状态：解码改造及验证工具已存在，完整直播转码应用尚未实现。** 现有构建关闭网络、缩放及大部分编码功能，不能直接用于完整转码。

## 目录

| 路径 | 用途 |
| --- | --- |
| `app/` | 后续转码程序 |
| `scripts/` | 整理后的构建与回归入口 |
| `docs/` | 项目现状和整理说明 |
| `experiments/decoder-20261016/ffmpeg-7.0.3/` | 固定版本的 FFmpeg 子模块；底层代码保留独立历史 |
| `experiments/decoder-20261016/` | 本地历史媒体、基线、报告、安装目录和交付包，均不上传 |
| `out/` | 新构建与测试结果，不上传 |

## 获取源码

```sh
git clone --recurse-submodules https://github.com/Nakiha/NakiLiveTranscode.git
cd NakiLiveTranscode
```

FFmpeg 固定在 `382492970c62d0c77ce0110d731d5ad78973b2b4`，来自 [Nakiha/FFmpeg](https://github.com/Nakiha/FFmpeg/tree/live/n7.0.3-async-early-output)。不要使用 `git submodule update --remote` 无意升级已测版本。

## 解码验证构建

Linux / WSL（安装 GCC、Make、NASM、pkg-config、Python 3）：

```sh
bash scripts/build-decoder.sh linux
```

Windows：从 MSYS2 UCRT64 运行（安装 UCRT64 GCC、Make、NASM、pkg-config、Python）：

```sh
bash scripts/build-decoder.sh windows
```

默认最多 8 个构建任务，可用 `FFMPEG_LIVE_BUILD_JOBS` 覆盖。输出位于 `out/build/<platform>-auto` 和 `out/install/<platform>-auto`。Linux 建议将整个新克隆放在 Linux 文件系统，以改善编译性能。

本机保留了历史媒体与原版 golden，构建后可运行：

```sh
bash scripts/test-decoder.sh linux
# 或在 MSYS2 UCRT64 中：
bash scripts/test-decoder.sh windows
```

每次回归使用新的结果目录，保留历史证据。新克隆不包含媒体；测试入口会在缺少基线时停止。获取方式见 [实验说明](experiments/decoder-20261016/README.md)。

## 已有文档与结果

- [异步接入与移植指导](experiments/decoder-20261016/ffmpeg-7.0.3/doc/live-async-guide-zh.txt)
- [自动模式接入](experiments/decoder-20261016/ffmpeg-7.0.3/doc/live-async-auto-zh.txt)
- [冻结测试与报告](experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async/validation/README.txt)
- [整理记录及后续工作](docs/workspace.md)

FFmpeg 及其附带代码遵循子模块内的许可说明；部署和分发应按实际构建配置检查其许可要求。
