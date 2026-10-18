# 独立性能测量入口

性能测量与 quick/full 功能回归分开。先在 Linux/WSL 构建服务，再运行：

~~~sh
bash scripts/build-service.sh

# 三种 VVenC 配置：生成短样本，重复比较首帧和阶段时延，另带 UDP 输入检查。
bash scripts/bench-service.sh latency --repeats 3

# 把 JSON 的 input 改成已有本地媒体；相对输入路径以 JSON 所在目录为基准。
bash scripts/bench-service.sh service --config /path/to/workload.json \
  --mode paced --seconds 60 --repeats 3

# 不主动按源时间轴限速，观察这个具体负载的吞吐。
bash scripts/bench-service.sh service --config /path/to/workload.json \
  --mode throughput --seconds 60 --repeats 3
~~~

service 接收现有服务配置，可用固定 1/2/4 输出、各路编码器、线程和画布组成单个负载。每次重复创建独立 controller/worker 与新输出文件；所有输出地址替换为新结果目录内的本地文件，原配置文件不会修改。输出格式和其他媒体配置沿用模板，pace 由 mode 指定，实际请求保存在 resolved-config.json。

观察到 EOF 可以提前结束；到 --seconds 上限则通过当前 job_id 请求停止并等待收尾。记录 stop_reason 和实际时长，不把短文件包装成长时稳定性验证。单次最多观察 72 小时，但没有自动循环输入，也没有自动执行 24/72 小时 soak；需要提供长度足够的媒体，另行设计直播网络和目标宿主机验收。

服务 JSON 示例：

~~~json
{
  "input": "input.mp4",
  "outputs": [
    {
      "id": "a",
      "output": "replaced-a.ts",
      "output_format": "mpegts",
      "encoder": "libx264",
      "width": 640,
      "height": 360
    },
    {
      "id": "b",
      "output": "replaced-b.ts",
      "output_format": "mpegts",
      "encoder": "libvvenc",
      "width": 640,
      "height": 360
    }
  ]
}
~~~

证据入口为新的 out/checks-benchmark-*/summary.json，负载结果在对应步骤 artifacts 中：

- SDK consumer gate、源码前后摘要、ELF/helper 哈希、配置与媒体 SHA-256、命令、退出码和超时；失败保留并返回非零。
- 每次重复的实际配置、最终服务状态、按输出的帧数/时延指标、输出媒体和独立解码检查。
- 每 250ms 采样 controller 及其后代的 CPU、RSS、线程、FD；JSONL 持续写入文件，内存中只保留聚合和相邻采样。身份以 PID 加启动时间区分，不向观测 PID 发送信号。

总体 elapsed/FPS 包含任务创建、起播和任务收尾；独立媒体检查及 controller 启停不在这个窗口。资源数字是采样进程总和，RSS 会重复计入共享页；轮询之间结束的进程可能漏记，不等于 cgroup 物理内存或内核峰值。服务直方图分位数仍是上界，保留普通 I/首 I/重复 I 与输出 key 分组，截止实际本地 mux/flush；不包括采集、客户端上传、wire time 或播放器显示。

--list 只展示计划；--build-dir、--bin-dir、--result-dir 与功能入口相同，结果目录必须新建于 out/ 下。latency 使用自己的固定小样本，接受 1–5 次重复；service 接受 1–10 次重复。通过表示测量和输出检查完成，不表示达到容量/SLO，不自动比较或承诺性能改善。

logger 仍有独立微基准：

~~~sh
python3 scripts/bench-logger.py --baseline-dir /path/to/saved/logger-source --repeats 3
~~~

baseline-dir 中应有匹配的 logger.c/logger.h；该工具使用内存 stdout 回调，不测真实磁盘或媒体时延。decoder 专项仍走 scripts/test-decoder.sh，需要本地历史媒体；冻结 n7 结果不能作为当前 n9 性能。
