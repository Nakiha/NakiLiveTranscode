# Docker 部署

也可直接下载 CI 验证过的镜像包并 `docker load`，同时保存配套的独立符号；操作见 [CI 与构建产物](ci.md)。离线解析轻量报告见 [崩溃诊断](crashes.md)。

```sh
git submodule update --init --recursive
docker compose build
docker compose up -d transcode
docker compose exec transcode naki-live-ctl health
docker compose logs -f transcode
```

多阶段构建 Ubuntu 24.04 + libx264 + VVenC + 自定义 FFmpeg，VVenC 共享库与 libstdc++ 一并装入运行层。运行时 UID/GID 10001，前台运行，不在容器中启动 cron。`init: true` 处理信号和孤儿子进程回收；退出重启采用 unless-stopped，停止宽限 30 秒。API 映射到宿主机 `127.0.0.1:8090`，UDP 15000 同样只在本机发布。访问其他机器时应自行配置鉴权/访问边界。

镜像入口是前台轻量监管程序 `service-launcher.py`，由它启动 C HTTP 控制进程。控制进程异常退出后按 0.25 秒起、最高 10 秒的退避重启，容器保持运行，原 worker 继续转码并由新控制进程接管。worker 的媒体和追踪文件 I/O 不经过监管入口。容器收到 SIGTERM 时会转交控制进程，明确停止全部 worker；监管入口最多等待 20 秒。整个容器重启、OOM 或主机重启仍可能结束转码；不能把此机制当成跨容器无缝恢复。

默认保留 1 个待命 worker。可在 Compose 覆盖服务 command，保留原 bind/run-dir 参数并追加 `--spare-workers 1`、`--worker-timeout 5`、`--worker-stop-timeout 3`。监管入口会将全部参数转发给 C 程序。私有接管端点位于按 run-dir 派生的容器 `/tmp` 目录；同一个容器里的控制进程重启会复用它。端点不放在日志维护卷内，维护容器不会接触它。

0.4.0 在每个 HTTP/worker 旁预启动独立 C 崩溃收集器，运行库增加 libunwind8。默认报告目录 `/var/log/naki/crashes`（0700，文件 0600）在 logs 卷内，单份 256 KiB、最多 32 份且合计不超过 16 MiB。外部 trace 日志维护脚本不会处理该子目录；崩溃收集器自行按配额保留报告。可用 `docker compose cp transcode:/var/log/naki/crashes ./crashes` 导出小型 JSON，无需搬运完整 core。

collector 通过指定 PID 的 PR_SET_PTRACER 和同 UID 访问抓栈，不使用 PR_SET_PTRACER_ANY，也不默认添加 SYS_PTRACE capability 或关闭 seccomp。实际内核/LSM/seccomp 策略仍可能拒绝，报告会保留寄存器并标记 attach 错误和 complete=false；应在目标环境验收。本轮不对简单监管入口本身做崩溃采集和故障注入。

`media` 卷挂到 `/data`，`logs` 卷挂到 `/var/log/naki`。任务配置中的路径指容器内路径；输出仍使用排他创建。可通过 `docker compose cp` 将输入和配置放入容器：

```sh
docker compose cp /path/to/input.mp4 transcode:/data/input.mp4
docker compose cp examples/docker.json transcode:/data/job.json
docker compose exec transcode naki-live-ctl start /data/job.json
docker compose exec transcode naki-live-ctl status
docker compose exec transcode naki-live-ctl stop
```

可用 `NAKI_HTTP_PORT`、`NAKI_UDP_PORT` 覆盖宿主机端口，默认分别为 8090、15000。

如果 cp 后文件所有权或权限不适合 UID 10001，需在宿主机/卷初始化时修正。改成 bind mount 时也要预先设置可写目录。容器内部 UDP 输入使用 `udp://0.0.0.0:15000`，不要绑定到容器回环地址后期望接收端口映射的数据。

0.8.0 运行层包含 SRT。接收 SRT 可用 `srt://0.0.0.0:15000?mode=listener&latency=50000`，沿用现有 UDP 端口映射；主动 SRT 推出使用接收服务器可达地址并选择 `output_format=mpegts`。RTMP 拉流/推流使用外部服务器地址及 `output_format=flv`；容器内的 127.0.0.1 指容器自身。示例见 [直播链路](live-mux.md)。当前服务不是多客户端 RTMP 发布服务器。

stdout 是每 10 秒汇总及生命周期 JSONL，由 Docker local 驱动轮转和压缩（20 MiB × 5，非阻塞缓存 4 MiB）。Docker 的缓存也可能丢日志，这部分不包含在应用 logging.dropped_* 中。Docker 内部日志文件不是可直接管理的 JSONL 追踪目录，不要由外部脚本改写。

启用逐帧明细时，应用对 logs 卷中的追踪文件轮转；外部定期执行一次维护：

```sh
docker compose run --rm log-maintenance
```

这个维护容器使用同一镜像，以相同 UID 处理 logs 卷中的已封存段。由宿主机 timer/运维调度触发，不和主进程争夺日志文件所有权。执行 `docker compose down` 保留卷；不要随意使用 `down -v` 删除媒体和日志卷。

正式部署需要按容器 CPU 配额配置解码预算和编码线程。当前默认 budget_slots=12 不自动识别所有 CPU quota；Compose 没有替用户猜测 CPU 配额。动态调度预算不是 CPU 限流器。

该镜像使用启用 GPL/libx264 的 FFmpeg 构建；分发时应同时满足所用源代码、许可和修改版本的对应源码提供要求。源码以本仓库子模块提交为准。

实际容器验证见 [进程版本验证记录](validation-processes-20261016.md) 和 [观测版本历史记录](validation-observability-20261016.md)。`tests/docker_smoke.py` 使用新生成的独立 Compose 项目名和测试卷，验证 HTTP 控制进程被杀后容器未重启、相同 worker 完成 75 帧输出；结束时仅回收该测试项目的容器及卷，保留镜像。
