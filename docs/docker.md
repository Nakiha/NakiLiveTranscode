# Docker 部署

```sh
git submodule update --init --recursive
docker compose build
docker compose up -d transcode
docker compose exec transcode naki-live-ctl health
docker compose logs -f transcode
```

多阶段构建 Ubuntu 24.04 + libx264 + 自定义 FFmpeg，运行时 UID/GID 10001，前台运行，不在容器中启动 cron。`init: true` 处理信号和孤儿子进程回收；退出重启采用 unless-stopped，停止宽限 30 秒。API 映射到宿主机 `127.0.0.1:8090`，UDP 15000 同样只在本机发布。访问其他机器时应自行配置鉴权/访问边界。

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

stdout 是每 10 秒汇总及生命周期 JSONL，由 Docker local 驱动轮转和压缩（20 MiB × 5，非阻塞缓存 4 MiB）。Docker 的缓存也可能丢日志，这部分不包含在应用 logging.dropped_* 中。Docker 内部日志文件不是可直接管理的 JSONL 追踪目录，不要由外部脚本改写。

启用逐帧明细时，应用对 logs 卷中的追踪文件轮转；外部定期执行一次维护：

```sh
docker compose run --rm log-maintenance
```

这个维护容器使用同一镜像，以相同 UID 处理 logs 卷中的已封存段。由宿主机 timer/运维调度触发，不和主进程争夺日志文件所有权。执行 `docker compose down` 保留卷；不要随意使用 `down -v` 删除媒体和日志卷。

正式部署需要按容器 CPU 配额配置解码预算和编码线程。当前默认 budget_slots=12 不自动识别所有 CPU quota；Compose 没有替用户猜测 CPU 配额。动态调度预算不是 CPU 限流器。

该镜像使用启用 GPL/libx264 的 FFmpeg 构建；分发时应同时满足所用源代码、许可和修改版本的对应源码提供要求。源码以本仓库子模块提交为准。

实际容器验证见 [验证记录](validation-observability-20261016.md)。`tests/docker_smoke.py` 使用新生成的独立 Compose 项目名和测试卷，结束时仅回收该测试项目的容器及卷，保留镜像。
