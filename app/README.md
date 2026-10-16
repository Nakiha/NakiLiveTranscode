# Linux 单路 C 服务

- `service.c`：libevent evhttp 控制面、任务生命周期、信号退出。
- `pipeline.c`：有界输入队列、异步解码 + 自动调度、filter、libx264、封装写出及逐帧追踪。
- `common.c` / `live.h`：yyjson 配置、JSONL 日志、状态与耗时摘要。
- `logger.c` / `logger.h`：独立日志线程、有界队列、丢弃计数和追踪文件轮转。
- `netstats.c` / `netstats.h`：FFmpeg TCP/UDP observer 的无日志原子计数与报表。
- `client.c`：libcurl 命令行 HTTP 客户端。

构建、使用及测量边界见 [服务说明](../docs/service.md)。
