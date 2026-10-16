# 0.8.0 直播链路验证

环境：本机 WSL Ubuntu 24.04，FFmpeg n9.0.2 fork `f5113758`、VVenC v1.14.0 fork `2b90643`，Linux C 服务。新证据目录 `out/live-mux-rg_j2qy_/`，不是沿用旧版本结果。

`tests/live_mux_integration.py` 已通过：使用真实本地 RTMP/SRT 连接发送 H.264 256×144/25fps/B3，VVenC `live_3b_2s` 输出 VVC；首视频时没有音轨，第一、第二 AAC 48kHz 双声道分别在源时间 2s/4s 出现，再分别停顿并恢复。TS 在 6s 加入未知私有 PES，验证后续 raw probe 的墙钟释放。

| 链路 | 独立解码视频帧 | AAC 各轨解码帧 | 最大观测视频进度间隔 |
| --- | ---: | --- | ---: |
| SRT → TS/VVC → SRT | 174 | 160 / 81 | 0.266s |
| RTMP → Enhanced FLV/VVC → RTMP | 172 | 152 / 91 | 0.267s |

每条链路只建立一次 source/sink 连接，任务 id/worker PID 不变；日志保存音轨注册事件、连续状态和完整实际写出帧追踪。前 151 帧验证关键帧位于 0/50/100/150，PTS 间隔 2s，完整组 3B、边界允许短组；取消尾部不宣称完整交付。视频进度间隔包括源 B 帧输出节奏和状态采样，不是单帧时延或线上 SLA。逐帧 `interleave_queue_ms` 全部小于测试上限 400ms，队列高水位未超过硬上限，有实际超时释放。

验证器持续读取并为后发现的流创建解码器，独立解码 VVC 和每条 AAC；没有使用固定初始轨道集合的 ffprobe 来宣称晚音轨帧数。普通 onMetaData 在默认 FLV demux 中也可能表现成文本流，验证只计音视频；服务不透传该数据流。

本地 EOF 补充：MP3 44.1kHz 透传到 FLV/TS、`audio_copy=false`、MP4 保持纯视频，均输出 75/75 视频帧且队列排空。C API 单测验证失败注册不改变流数量、注册不写 header/连接、DTS 恒定且无新输入时 50ms 墙钟释放、4包/16字节测试上限、EAGAIN 保留输入、超大包拒绝、跨 time_base 排序、禁止混用通用交织队列。

同次源码的 decoder、基础 service、VVC、PTS GOP、observability、process recovery 回归通过。测试不等同现网服务器/播放器兼容性、长时间稳定性或高规格容量验证；SRT 内部 UDP 未纳入现有 TCP/UDP observer。

补充实测目录 `out/live-mux-aje7fsd6/`：RTMP 输入改为 Enhanced FLV/HEVC，SRT 保持 H.264/TS；两者仍通过 VVC、两条晚 AAC、同连接和交织测试。该输入组合已固化到回归脚本，覆盖 Enhanced FLV 的 HEVC 解封装入口。
