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

## 接入回归记录

Linux amd64 功能回放使用实拍素材派生的 60 帧 H.264 文件，服务以示例 CCRF 参数完成 MPEG-TS 写出。FFmpeg 全量解码得到 60 帧，逐帧 PTS 与输入一致。源 I 位于第 0、7、9、51 帧，输出对应首帧 IDR 和三个普通 I；60 帧请求 QP 均与实际 slice QP 一致。按服务写包日志中的视频 AU 字节与输出 DTS 计算，滑动一秒峰值为 925256 bit，配置为 1100000 bit/s。此短样本仅作功能回归，目标机长跑负责性能与码控验收。

## 里程碑

| 阶段 | 状态与验收 |
| --- | --- |
| 服务接入 | C 后端、指定帧普通 I、外部逐帧 QP、PTS/DTS 和帧类型确认。强机从上面的配置复现。 |
| 码率上限 | 目前采用反馈预测。固定 CRF，改变 maxrate，对静态、运动、切场和连续 I 逐一测每个滑动 1 秒的峰值、超限次数和最大超限百分比。分别按媒体 DTS 和实际写出时钟统计，计入视频头；另列音频、容器和网络开销。客户的窗口、突发容忍度及是否包含封装需确认后才能验收。 |
| 直播性能 | 固定机器、核配额和并发，对比既有 H.265 模板。报告每帧编码时延、ingress 到 mux/flush 的 p95/p99、最长连续出流间隔、丢帧数和同画质实际码率。 |
| VVC 工具 | 先拆开评估 AMVR、依赖量化和 MTT 的画质收益与尾延迟。MMVD、SBT、LADF 在当前内核中对应的 SPS 位固定为零；实现需覆盖模式决策、重建与码流一致性。腾讯样本的未来参考、RPL 与色度参数另作对照；引入未来参考时重新量化等待预算。 |
| 多路与发布 | 审核上游全局状态、跨实例资源释放与 SIMD；做长稳、压力、异常退出和完整服务回归后，再决定默认档位。 |

## 编码器演进顺序

1. **冻结强机对照。** 用同一段已准备的 540×960 输入，保持 `pace=true`，连续跑三次 `live_ld`。每次独立启动一个任务、生成新输出。记录 CPU、线程与核配额、实际输入节拍、输入/输出帧数、每帧 `encode_ms`、`ingress_to_mux_ms`、相邻 `mux_flush_done_monotonic_us` 的最大间隔及两端帧号。单列首帧、周期 I 和普通帧。最终交付门槛由同机同负载下的客户 H.265 模板确定，模板缺失时保留空项。
2. **先把接口和标量 C 参考实现做对。** 服务拥有媒体时钟、帧策略与写出反馈；内核拥有参考图像、模式选择、重建与熵编码。给每个新增工具留独立开关和实际使用计数。先测现有 AMVR、依赖量化、深度 1 MTT 的单项增量，再按同画质码率收益与尾延迟安排 MMVD、SBT、LADF。GOP、参考关系及 WPP 的变更单独测试。每个工具独立提交，默认档始终可运行。
3. **每项同时过正确性与直播成本两道检查。** 用能触发该工具和不触发它的短片验证，保存码流、语法证据和重建摘要。测同画质码率、逐帧 p95/p99、最长写出间隔、每帧尝试/选中的候选及热点耗时。可以暂时保留较慢的标量实现作实验档；默认直播档需要有明确收益并满足已约定的成本预算。真实 30 fps 的输入间隔约为 33.33 ms，端到端延迟上限与允许出流空档需单独定义。
4. **按测量结果加速。** 优先减少无收益的搜索、等待、全帧复制和热路径分配。占用显著且稳定的像素/变换内核可立即做 SIMD，保留标量入口与运行时分派。对要求位精确的内核，用相同输入逐块比较标量与 SIMD 输出，覆盖未对齐地址、不同 stride、边缘块和尾部宽度，再做整流重建回归。改变候选排序或提前终止会改变码流，需重新测率失真和尾延迟。

## VTM 与 FFmpeg 的离线验收

服务计时结束后，保留带 PTS 的容器作时序比较；从同一容器无损抽出 Annex B 码流用于像素比较。优先固定 VTM-24.0，以便与既有腾讯 trace 对读，记录实际 tag、提交和构建选项。下列命令适用于这份 8-bit、可见尺寸 540×960 的 MPEG-TS 输出。用新目录保存大文件，DecoderApp 可替换为本机实际路径。

```sh
ffmpeg -v error -i output.ts -map 0:v:0 -c:v copy -f vvc output.266
DecoderApp -b output.266 -o vtm.yuv > vtm-decode.log 2>&1
ffmpeg -v error -xerror -err_detect explode -i output.266 -map 0:v:0 \
  -fps_mode passthrough -pix_fmt yuv420p -f rawvideo ffmpeg.yuv
wc -c vtm.yuv ffmpeg.yuv
sha256sum vtm.yuv ffmpeg.yuv
cmp vtm.yuv ffmpeg.yuv
```

两端统一输出到显示顺序、可见裁剪窗口、8-bit planar 4:2:0。每帧为 777600 字节，600 帧应为 466560000 字节。发现差异时先检查输出格式与顺序，然后定位首个不一致的显示帧、分量和 POC。开发改动重建、预测、量化或滤波的工具时，再加入编码器本地重建做三方逐帧比对，避免编码端参考图发生漂移。短窗语法 trace 核对 IDR 与普通 I、TId、参考关系和工具实际命中；对 LADF 这类由样本推导的工具要核对滤波推导与重建。首次复现可完成 FFmpeg 和计时后再补 VTM，缺少的验证明确记为待验。通过这些样片属于互通与回归证据，完整符合性另按 ITU-T H.266.1 执行。
