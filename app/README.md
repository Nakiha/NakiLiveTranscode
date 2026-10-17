# Linux 单输入 C 服务

- `service.c`：libevent evhttp 控制面、任务生命周期、信号退出。
- `pipeline.c`：有界输入队列、异步解码 + 自动调度、8/10-bit filter、libx264/libvvenc、封装写出及逐帧追踪。
- `input_probe.c` / `input_probe.h`：带缓存的首条 H.264/HEVC 视频就绪探测；其他音轨/数据可继续晚发现，不在初始化阶段固定完整流集合。
- `frame_policy.c` / `frame_policy.h`：纯策略核心，包含严格时间轴、接受后推进的 I 调度、因果选择和有界在途身份。
- `frame_policy_config.c` / `frame_policy_config.h`：可选帧策略组的严格 JSON 合同；缺省兼容 legacy，过载阈值必须显式提供。
- `frame_policy_runtime.c`：worker 独立轮询、在途年龄和过载失败；媒体 owner 仍独占 filter/encoder/mux。
- `watermark_config.c` / `watermark.c`：单水印严格配置、后台有界资源准备、required 门槛及 SDR 8/10-bit 合成；资源线程不接触主媒体 graph/encoder/mux。
- `common.c` / `live.h`：yyjson 配置、JSONL 日志、状态与耗时摘要。
- `logger.c` / `logger.h`：独立日志线程、有界队列、丢弃计数和追踪文件轮转。
- `netstats.c` / `netstats.h`：FFmpeg TCP/UDP observer 的无日志原子计数与报表。
- `client.c`：libcurl 命令行 HTTP 客户端。

构建、使用及测量边界见 [服务说明](../docs/service.md)。

固定多输出补充：output_config.c 解析 outputs 与每路预算；branch_queue.c 持有有界帧/音频参数引用；video_filter.c 准备独立滤镜图。pipeline.c 共享 demux/decode，分支 owner 独占 graph/encoder/mux，并在固定输出下处理输入分辨率/位深变化。单路失败只取消该路；common.c 输出各路状态与实际写出指标。完整边界见 ../docs/branching-contract.md。
