# 日志、时延和网络观测验证：2026-10-16

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

版本：服务 0.2.0，FFmpeg 子模块 `cbf2341520725a4f8b217052f864aa7dd3028a8d`（libavformat 61.2.100，网络观测 API 1；解码 API 2 保持不变）。验证平台为本机 WSL Ubuntu 24.04 和 Docker Desktop Linux Engine 28.3.0。

| 验证 | 结果 |
| --- | --- |
| 文件与实时 UDP 完整转码 | H.264/HEVC 输入、缩放、H.264 编码、MPEG-TS/MP4 输出；帧数、尺寸和时长正确 |
| 时延统计 | 全帧累计值与逐帧明细核对均值、最大值、慢帧计数和 p50/p95/p99 桶上界；窗口计数合计等于完整输出计数 |
| I 帧与采样 | 解码帧类型和 KEY 标志与独立 ffprobe 解码结果一致；首个/后续 I 帧分开；明细采样后仍保留全部 I 帧和输出关键帧尝试记录 |
| 控制定时器 | 实际默认 10 秒 heartbeat；加速的 1 秒测试窗口互不重叠；任务结束后仍有 idle heartbeat |
| 写出阻塞 | 用不读数据的真实 TCP 接收端制造阻塞；状态和 HTTP 仍响应，write_in_progress_ms 可见 |
| 有界日志队列 | 用已填满的 stdout pipe 阻塞日志线程，生产者仍能继续；队列上限、保留状态名额、丢弃计数及停止封存通过 |
| 文件轮转与维护 | 小段大小/时间轮转；封存 gzip 内容相同；7 天过期和总空间清理；排除活跃文件、无关文件和符号链接 |
| 网络埋点 | TCP 实际发送字节与独立接收文件大小一致；HTTP/TCP 接收、UDP 后台 RX、直接 RX、直接 TX 和后台 TX 均有实际数据 |
| 错误语义 | native 回调故意改变 errno，TCP readiness 超时仍返回原 ETIMEDOUT；事件时间顺序正确 |
| 关闭与异常 | 坏输入、已有文件保护、取消、无包 UDP 和 SIGTERM 保持通过 |
| 禁用网络构建 | 单独最小 --disable-network 共享库构建通过，observer API 不额外引入网络依赖 |
| Docker Compose | 实际构建并运行；UID/GID 10001、healthcheck、本地日志驱动、75 帧完整转码、独立维护容器共享日志卷、SIGTERM exit 0；测试专用资源已回收 |

普通构建日志：`out/test-logging.log`、`out/test-observability.log`。最近的完整媒体用例在 `out/service-test-j5fgle9g/`，最近的观测用例在 `out/service-test-e9o53ggx/`。

应用及 yyjson 以 ASan/UBSan + Leak 检查重跑，均通过：`out/test-logging-asan.log` / `out/service-test-9ifqas7m/`，`out/test-observability-asan.log` / `out/service-test-y9z6a8ua/`。这些检查未将 FFmpeg、libevent、libcurl、libx264 共享库重新插桩，未执行 TSan。

Docker 镜像 `naki-live-transcode:local` 验证产物在 `out/docker-smoke-q66iheyy/`，含 Compose 命令、容器 JSONL、输出媒体和 COMPLETE.json。构建日志为 `out/build-docker.log`，最小禁网构建日志为 `out/build-no-network.log`。以上均为本机产物，不加入 Git。

这不是 7×24 小时 soak test，也没有证明多路故障隔离或生产容量。仅 socket 收发计数和时间边界，不实现帧与网络包逐一关联，不声称采集到显示时延。
