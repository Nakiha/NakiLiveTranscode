# VVenC 实验链路

源码为 [Nakiha/vvenc](https://github.com/Nakiha/vvenc)，上游 [fraunhoferhhi/vvenc](https://github.com/fraunhoferhhi/vvenc)。独立子模块 `third_party/vvenc` 固定 v1.14.0，提交 `9428ea8636ae7f443ecde89999d16b2dfc421524`。FFmpeg 继续固定 n9.0.2 fork，不需要为了启用编码器改上游 wrapper。

构建保留 VVenC 默认的内部 nlohmann JSON 支持：v1.14.0 的单遍码控前瞻缓存依赖该路径，关闭后即使不写 stats 文件也会在排空时失败。它仅是编码库内部依赖，服务的 HTTP/日志 JSON 继续使用 yyjson。

运行请求见 [vvc.json](../examples/vvc.json)：`encoder=libvvenc`、`encoder_preset=faster`，默认 1.5 Mbps、2 个编码线程。应用、HTTP 控制、IPC、崩溃采集仍为 C；VVenC 是独立 C++ 共享库，容器含 libstdc++。filter 根据编码器协商 YUV420P10LE，覆盖 8-bit 转 10-bit 与原生 10-bit 输入；这不等于把 SDR 自动转换为 HDR，色彩元数据从输入传递。

当前开放 MPEG-TS（含 UDP）和本地 MP4，VVC + FLV 在请求阶段拒绝。普通 MP4 要在 EOF/trailer 后完成索引，不适合直接作为持续直播交付格式。仍是纯视频、固定规格实验；输入编码限 H.264/HEVC，不接受 VVC 回灌。

## 时间与帧关联

VVenC 的默认 GOP、单遍码率控制含前瞻和重排，不能套用 x264 的 zerolatency 参数。默认 intra period 为 2 秒，具体帧间隔由库按 GOP 调整。preset 改变速度/质量，不保证任何机器上实时，也不保证首帧时延上限。

FFmpeg wrapper 的 DTS 默认以一帧为一 tick，服务为 VVenC 使用名义帧率的倒数作为编码时间基（包括 30000/1001），再换算到容器时间基。输入 PTS 因此量化到名义帧周期；非单调或量化后重复的 PTS 显式失败，当前不承诺任意 VFR 输入兼容。frame rate 不由请求 `fps` 强制覆盖，`fps` 仅在输入缺失帧率时回退。

每个输入 Stamp 沿 packet/frame opaque_ref 进入 filter，编码后按 packet PTS 在 256 项有界 ledger 中找回。不能按输出顺序关联：VVC 输出顺序可能不是显示顺序。正常 EOF 排空 encoder 后必须清空 ledger，否则任务失败；容量满也明确失败，不丢统计、不扩成无界队列。first I 在解码显示顺序中标记，重排后仍保留该身份；输入 I 与 VVC 输出关键帧分别统计。

`encode_ms` 从该帧送入编码器之前到对应编码包取出，包含编码调用、前瞻、内部排队、重排和后续喂帧等待。它是驻留时间，不是单帧 CPU 用时；多个帧的驻留时间可以重叠。每帧成功写出均更新各阶段及输入 I 分组的累计/窗口统计，逐帧日志只控制明细量。每次任务仍无条件输出首视频包、首输出关键帧的 `first_frame`，保留 HTTP 完整请求 handler 到 mux flush 的启动链路。上述值不包含客户端请求网络、编码器之前的探流包等待、源采集和播放器显示；探流/初始化耗时会体现在请求到首输出指标中。

## 修改、CI 与符号

`bash scripts/build-service.sh` 先在 Linux cache 编译 VVenC，再启用 FFmpeg `--enable-libvvenc`；可独立运行 `bash scripts/build-vvenc.sh`。修改子模块后先在 fork 提交并 push，再更新主项目 gitlink。不要使用 `submodule update --remote` 意外换 pin。`NAKI_SKIP_FFMPEG=1` 仅适用于依赖已构建的应用开发，不会重编 VVenC。

依赖 CI 一次构建 VVenC + FFmpeg，缓存键含两个子模块提交及构建配方。SDK 包含独立 `vvenc-symbols/`，服务发布包中的对应目录为 `vvenc/`，与 `app/`、`ffmpeg/` 并列。库从同一次构建导出 DWARF 后剥离运行库，保留 Build ID 和 unwind；manifest 记录准确 VVenC 源码提交。离线解析同时提供三个目录：

```sh
python3 scripts/symbolize-crash.py crash.json \
  --symbols symbols/app --symbols symbols/ffmpeg --symbols symbols/vvenc
```

`tests/vvenc_integration.py` 生成 H.264/HEVC 的 8/10-bit B 帧输入，覆盖缩放、MPEG-TS、MP4、UDP、分数帧率、重排与单帧 EOF，使用构建出的 n9 ffprobe 原生 VVC decoder 独立解码并计数，验证输入帧关联、各阶段与 I 帧统计、无明细时的首帧日志。CI 另验证运行镜像能装载并执行 VVenC，以及 VVenC Build ID 到函数/源码行的离线解析。
