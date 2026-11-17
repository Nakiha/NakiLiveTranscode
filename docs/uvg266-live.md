# uvg266 直播分支与强机复测

服务直接链接 `third_party/uvg266` 固定的 8-bit 静态库。HTTP 配置选择 `encoder=libuvg266`，输入仍经过现有解码、帧策略和滤镜，输出仍经过 PTS 对应、写包和 flush 计时。直播档固定 540×960、30 fps、ultrafast、WPP、OWF=0、仅过去参考。每个 worker 进程同时使用一个 uvg266 实例，上游打开或关闭实例会修改全局策略表和码控状态。控制层和本次新增的服务适配器使用 C；上游 x86 的部分 AVX2 内核使用 C++。

## 在 Linux x86_64 上运行

安装主 README 的编译依赖和支持 H.266 解码的 FFmpeg。使用新输出目录运行，通过子模块固定对应的编码器版本。

```sh
git clone --branch feat/uvg266-live --recurse-submodules https://github.com/Nakiha/NakiLiveTranscode.git
cd NakiLiveTranscode
bash scripts/build-service.sh
./out/bin/naki-live
```

另一终端在同一目录准备 20 秒本地输入。示例按中心裁剪为 9:16，统一到 30 fps，按指定帧号插入源 I 帧。准备阶段单独转为 H.264 以便复核帧策略，评画质时以此输入的解码结果作参考。原始媒体由测试者本地提供。

```sh
ffmpeg -v error -i source.mp4 -t 20 -an \
  -vf "fps=30,crop='min(iw,ih*9/16)':'min(ih,iw*16/9)',scale=540:960,setsar=1" \
  -c:v libx264 -pix_fmt yuv420p -bf 0 -g 60 -x264-params scenecut=0 \
  -force_key_frames 'expr:eq(n,7)+eq(n,8)+eq(n,51)' /tmp/sample.mp4
./out/bin/naki-live-ctl start examples/uvg266-live-540x960.json
./out/bin/naki-live-ctl status
```

任务结束后独立解码并查看关键帧和普通 I 帧。

```sh
ffmpeg -v error -xerror -i /tmp/uvg-live.ts -map 0:v:0 -f null -
ffprobe -v error -select_streams v:0 -show_frames \
  -show_entries frame=pts_time,key_frame,pict_type -of csv /tmp/uvg-live.ts
```

示例使用 `rate_control=constrained_crf`、CRF 32、maxrate 1100000 bit/s。控制器按复杂度与过去一秒的已写和待写字节选择逐帧 QP，并记录请求 QP 和实际 slice QP。跟随源 I 的档位首帧 IDR，后续请求生成普通 I slice，保留 PTS 与参考历史。普通 I 不提供随机接入。需要周期随机接入的对照可用原生 bitrate 路径并省略 intra 策略。实验档 `live_ld_tools` 在同一过去参考结构上同时打开 AMVR、依赖量化和一层 MTT，可用于功能对照。

## 里程碑

| 阶段 | 状态与验收 |
| --- | --- |
| 服务接入 | C 后端、指定帧普通 I、外部逐帧 QP、PTS/DTS 和帧类型确认。强机从上面的配置复现。 |
| 码率上限 | 目前采用反馈预测。固定 CRF，改变 maxrate，对静态、运动、切场和连续 I 逐一测每个滑动 1 秒的峰值、超限次数和最大超限百分比。分别按媒体 DTS 和实际写出时钟统计，计入视频头；另列音频、容器和网络开销。客户的窗口、突发容忍度及是否包含封装需确认后才能验收。 |
| 直播性能 | 固定机器、核配额和并发，对比既有 H.265 模板。报告每帧编码时延、ingress 到 mux/flush 的 p95/p99、最长连续出流间隔、丢帧数和同画质实际码率。 |
| VVC 工具 | 先拆开评估 AMVR、依赖量化和 MTT 的画质收益与尾延迟。MMVD、SBT、LADF 在当前内核中对应的 SPS 位固定为零；实现需覆盖模式决策、重建与码流一致性。腾讯样本的未来参考、RPL 与色度参数另作对照；引入未来参考时重新量化等待预算。 |
| 多路与发布 | 审核上游全局状态、跨实例资源释放与 SIMD；做长稳、压力、异常退出和完整服务回归后，再决定默认档位。 |
