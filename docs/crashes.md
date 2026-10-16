# C 进程崩溃诊断

## 当前能收集的记录

worker 的 ordinary pipeline_error 表示媒体调用返回错误；SIGSEGV/SIGABRT 等原生崩溃则由存活控制进程观察退出，输出 `worker_crash`、`worker_exited` 和最终任务状态。记录包含 job/request ID、PID、已知退出码/信号、最后快照和快照年龄。`core_dumped` 是 wait 状态的内核标记，不证明 core 文件已保存在可访问目录。接管的进程通常不是新控制端的内核 child，`exit_status_known=false`，不能编造信号或 core 标记。

控制进程崩溃由容器入口记录 `control_restarting`，包含 PID、退出结果、已知信号和重启退避；worker 保持转码。裸机直接运行二进制时没有该外部记录，需由服务管理器收集。控制端自身日志队列、IPC、Docker 日志都可能丢记录；这些 JSONL 不是崩溃现场的替代品。

本程序没有安装 SIGSEGV 栈回溯处理器。崩溃可能发生在 allocator、logger、codec 或其持锁区域，signal handler 里调用 malloc、stdio、JSON、pthread mutex 或普通 logger 不安全。内核 core + 外部采集比在不一致进程内继续做复杂工作更适合此服务。

## 宿主机配置 core

先读取实际环境，不由应用改系统全局设置：

```sh
ulimit -Sc
ulimit -Hc
cat /proc/sys/kernel/core_pattern
cat /proc/sys/kernel/core_pipe_limit
```

如果宿主机已经使用 systemd-coredump，按发行版配置其存储、单份处理/存储大小及总配额，使用 `coredumpctl list`、`coredumpctl info <PID>`、`coredumpctl dump <PID> --output=<new-core-path>` 或 `coredumpctl debug <PID>` 检查。不要不加检查地覆盖已有 core_pattern；这会影响整台主机的其他服务。

普通文件形式的 core 需要可写目录、非零合适的 core 限制和有区分度的名称，例如由管理员选择的 `core.%e.%P.%t`。管道型 core_pattern 由宿主机收集器处理，RLIMIT_CORE 不一定限制其采集，因此必须配合收集器自身的大小/并发/保留配置。不能把 `ulimit -c 0` 一概当作绝不会采集。

Docker 的 core_pattern 由内核/宿主机环境决定；容器 ulimits 只是一部分。若选择文件落盘，路径还涉及崩溃进程的 mount namespace、UID 和持久卷权限；若选择管道收集器，它在宿主机的初始命名空间执行。真实 Linux 生产机应由宿主机管理采集。Docker Desktop 使用另一个 Linux VM，WSL 本身又可能指向 `/wsl-capture-crash`；本次没有修改任一环境的全局规则，也没有验收生产 core 收集链路。

主动 SIGKILL、部分 OOM kill、容器/主机消失不会产生可供分析的普通 core。此类事件需结合退出码/信号、Docker OOMKilled、宿主机内核/OOM/cgroup 记录判断；exit 137 单独不能证明 OOM。

## 每次发布保留匹配的分析材料

为 C 服务、worker、自定义 FFmpeg 和 x264 保留相同构建的 ELF/调试符号、Build ID、源码提交/修改记录、编译选项、镜像 digest 与依赖版本。`RelWithDebInfo` 为应用保留调试信息；FFmpeg 的构建缓存中有未剥离的共享库，但安装/发布产物是否保留符号要逐一核对，不能用系统 FFmpeg 的符号分析自定义 fork。

```sh
file -L out/bin/naki-live-worker
readelf -n out/bin/naki-live-worker
file -L /path/to/ffmpeg/lib/libavcodec.so.61
readelf -n /path/to/ffmpeg/lib/libavcodec.so.61
```

保留原始未剥离 ELF，或在 strip 前用 objcopy 导出独立 debug 文件，并按 Build ID 归档。源码提交号不能替代匹配的二进制；同一提交使用不同编译选项也会产生不同地址。不要把 core、媒体、私有路径或带凭据的运行配置提交到公共仓库。core 可能含帧、URL 和凭据，应设置访问权限、磁盘配额与保留期限。

在匹配的镜像/调试环境离线运行，例如：

```sh
gdb -batch \
  -ex 'set pagination off' \
  -ex 'info sharedlibrary' \
  -ex 'info threads' \
  -ex 'thread apply all bt full' \
  /exact-build/naki-live-worker /new/core-file > /new/backtrace.txt
```

共享库必须同样匹配；从容器外分析时配置正确的 sysroot、共享库和 debug 文件搜索路径。全线程栈和局部变量用于定位阻塞/损坏，栈被内存破坏时结果也可能不可靠。开发回归继续使用 ASan/UBSan，补足越界、use-after-free 等在 crash 前发生的错误；这些测试不等于生产所有依赖都已被插桩。

建议先在专用测试实例触发一次可控崩溃，验收“结构化摘要 → 可访问 core → Build ID 对应 → 能解析自定义 FFmpeg 的线程栈”。当前验证覆盖 SIGSEGV 隔离和摘要日志，未声称完成真实 core 的生成、存储和符号化验收。

参考：[core(5)](https://man7.org/linux/man-pages/man5/core.5.html)、[signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html)。
