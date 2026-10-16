# 轻量 C 崩溃采集（0.4.0）

默认不依赖、也不先生成完整 core。HTTP 控制进程和每个 worker 在启动应用线程前，各预启动一个独立的 `naki-crash-collector`。真正抓栈时由 collector 临时派生 capture 子进程；yyjson、libunwind 和报告文件 I/O 都在目标进程之外。监管入口依照本轮假设不纳入故障注入。

## 现场交接与退出

捕获 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT。handler 从 siginfo/ucontext 复制原始寄存器、信号、PID/TID、适用时的故障地址和单调时间，以一个固定 384 字节消息发送到预先建立的非阻塞 socket。失败不重试堆积；不调用 yyjson、logger、malloc、stdio、互斥锁或 unwinder。多线程同时故障由 lock-free guard 只选一份首发现场，其余线程也有退出期限。链接时提前解析应用符号，handler 关闭 sanitizer 插桩，避免把普通运行时诊断逻辑带进致命信号路径。

handler 最多等待配置的期限（默认 2 秒）。收到结果、通道失联或期限到达后，对已经发生致命错误的本进程设置不可 dump，再恢复并重发原始致命信号；正常采集路径保留 signalled exit，不产生完整内核 core，也不修改机器的全局 core_pattern。它不会返回继续转码。SIGKILL/OOM/主机消失不经过这些 handler。

若安全策略连设置不可 dump 都拒绝，handler 改用 _exit(128+signal) 避免意外生成大 core；此时父进程看到普通退出码，原始信号以已收集报告为准，不伪称 signalled exit。

主线程和本应用创建的 logger、媒体、demux 线程尝试注册 128 KiB 备用信号栈与 guard pages，已有备用栈则保留。FFmpeg/第三方自己创建的线程仍使用其原有配置；普通 SIGSEGV 可在正常栈上进入进程级 handler，但不能保证未注册备用栈的线程发生栈溢出时也能送出记录。

## 独立抓栈与报告

collector 通过 peer credentials 核对初始通道身份，以 pidfd 监视目标；只注册指定 collector PID 的 PR_SET_PTRACER，不放开 PR_SET_PTRACER_ANY。抓栈子进程逐线程 seize/interrupt、用 libunwind-ptrace 远程展开，故障线程从消息中的原始寄存器开始，避免把 handler 自己的 poll 栈误当崩溃位置。模块记录 basename、Build ID、映射范围和可解析时的 ELF 地址。不读取并导出完整堆、帧缓存、原始栈字节、环境或命令行；寄存器、模块名及函数名仍应按诊断资料控制访问，不自动上传。

默认最多 32 线程、每线程 48 帧、96 个可执行映射；模块 maps 文本读取至多 256 KiB，单段 ELF note 至多 64 KiB。远程 word 读取默认预算 1 MiB；这是 unwind accessor 的目标内存读取预算，不是所有文件 metadata I/O 的总量。栈损坏、线程消失、优化/汇编缺少 unwind 信息、ptrace 权限限制等都会使结果不完整。查看 complete、capture_result、truncated、各 limit_hit 和 unwind_or_attach_error，不能把固定现场或不完整栈当成完整 dump。

collector 把抓栈期限限制在总配置时间的 70%，自己监视 capture 子进程；超时杀掉抓栈子进程，使其 ptrace attachment 释放，输出 capture_timeout 的固定现场回退报告。抓栈子进程另有父进程死亡保护。如果 SIGKILL 后在额外回收期限（配置总时间的 10%，最多 100 ms）内仍不能确认抓栈子进程退出，collector 通过原目标 pidfd 强制结束已崩溃目标，并标记 capture_timeout_forced_target_exit。极端内核不可中断状态不能由用户态承诺严格墙钟期限。

失联/死亡的 collector 不会让 handler 无限等待；此时可能只有外部 worker_crash/control_restarting 摘要，没有新的 JSON 报告。权限受限也不在 handler 里尝试自回溯，仍保留固定寄存器并记录 attach 错误。PR_SET_PTRACER 注册失败不阻止普通服务启动；实际捕获能力应在目标内核/LSM/seccomp 环境验收。

## 位置、大小与保留

`/health` 返回 crash_report_dir、crash_collector_pid、crash_capture_available、crash_ptracer_registered。PID/注册成功不等于所有线程可抓栈；available 只检查当前通道是否仍可用，不是完整采集验收。worker 快照也带其 collector PID。

默认目录 `<run-dir>/crashes`。目录要求服务 UID 所有且权限 0700，文件 0600。如果 WSL 的 Windows 挂载目录不能实现该权限，自动目录退到原有私有 Linux state-dir 下的 crashes，以 health 返回路径为准；显式 --crash-dir 必须满足权限要求。建议现网显式指定 Linux 持久目录。文件名 `crash-<pid>-<monotonic_us>-<tid>.json`，可按 PID/时间及 worker job_id 关联现有 JSONL。

| 参数 | 默认 | 边界 |
| --- | --- | --- |
| --crash-dir | 自动 | 已有父目录下的私有目录 |
| --crash-timeout-ms | 2000 | 100..10000 ms |
| --crash-max-report-bytes | 262144 | 4096..2097152 字节，包含末尾换行 |
| --crash-max-reports | 32 | 1..256 个封存报告 |
| --crash-max-total-bytes | 16777216 | 不小于单份上限，最大 1 GiB |
| --crash-max-threads | 32 | 1..32 |
| --crash-max-frames | 48 | 1..48，每线程 |
| --crash-max-read-bytes | 1048576 | 4096..16777216 字节 |

同名 NAKI_CRASH_* 环境变量也可配置，例如 NAKI_CRASH_TIMEOUT_MS、NAKI_CRASH_MAX_REPORT_BYTES；控制进程把最终配置传给后续 worker。已接管的老 worker 保留原配置。自定义 worker heartbeat/stop 期限最好大于 crash capture 期限，否则监管器可能提前终止采集。

JSON 超过单份上限时逐步减少每线程输出帧数，必要时只保留固定现场并明确标记 report_size_limit_hit，绝不把原始 JSON 截断成坏文件。报告经临时文件完整写入后原子发布；保留锁下仅删除本目录内符合本程序命名的普通、同 UID 报告，从旧到新清到份数/总量上限，不跟随 symlink。所有 writer 都持锁至发布，因此下一次持锁 writer 也会安全清理匹配本程序命名的普通、同 UID 遗留 tmp。锁等待最多 100 ms。没有默认 fsync、自动上传或压缩；无关文件与磁盘整体配额不属于已封存报告上限。磁盘满/不可写时可能没有报告，不阻碍目标按期限终止。

## 验证与离线定位

安装系统 libunwind-dev 构建；部署需要 libunwind8，collector 与两个应用二进制放在同一目录。全部为 C。当前实际验证 Linux x86_64；代码有 AArch64 寄存器分支，但未在 ARM 机器上验收。

`python3 tests/crash_integration.py` 注入真实非法访问，核对 fault_pc、寄存器、原始函数链以及 addr2line 源码定位；覆盖非主线程、持锁、备用栈溢出、双线程同时故障、并发报告、限额、抓栈卡死、collector 死亡/暂停、ptrace 拒绝、worker 与 HTTP 崩溃及同 PID 接管。Docker 测试也注入这两类角色的 SIGSEGV，并要求能拿到线程栈，不默认添加额外 capability。见 [本次验证记录](validation-crashes-20261016.md)。

导出 JSON 后，使用模块 Build ID 核对本次构建保留的原始 ELF/独立 debug 文件，再用 frame 的 elf_address 定位，例如：

```sh
readelf -n /exact-build/naki-live-worker
addr2line -f -C -i -e /exact-build/naki-live-worker 0x<elf_address>
```

共享库帧要使用该模块匹配的库/符号文件。报告里的 function 是现场能解析到的符号，缺少符号不代表没有栈帧。没有完整堆和局部变量，不能还原全部内存破坏原因；开发侧继续使用 ASan/UBSan 和最小复现。

## 当前能收集的记录

worker 的 ordinary pipeline_error 表示媒体调用返回错误；SIGSEGV/SIGABRT 等原生崩溃则由存活控制进程观察退出，输出 `worker_crash`、`worker_exited` 和最终任务状态。记录包含 job/request ID、PID、已知退出码/信号、最后快照和快照年龄。`core_dumped` 是 wait 状态的内核标记，不证明 core 文件已保存在可访问目录。接管的进程通常不是新控制端的内核 child，`exit_status_known=false`，不能编造信号或 core 标记。

控制进程崩溃由容器入口记录 `control_restarting`，包含 PID、退出结果、已知信号和重启退避；worker 保持转码。裸机直接运行二进制时没有该外部记录，需由服务管理器收集。控制端自身日志队列、IPC、Docker 日志都可能丢记录；这些 JSONL 不是崩溃现场的替代品。

0.4.0 上述固定现场 handler 与普通日志隔离，抓栈和 JSON 都在外部进程。崩溃可能发生在 allocator、logger、codec 或其持锁区域，不在不一致进程内继续做复杂分析。

## 完整 core 的可选深度调试参考

以下是保留的宿主机排查参考，不是 0.4.0 默认采集路径。当前 handler 在终止前抑制完整 core；若专项调试确需 core，需使用相应调试构建/终止策略并验收宿主机配置，本版没有默认开启完整 core 的开关。

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

现已接入按 Build ID 归档和离线解析，CI 构建方式见 [CI 与构建产物](ci.md)。下载与部署镜像配套的 `symbols-linux-amd64.tar.gz`，在 Linux/WSL 安装 Python 3.11+ 和 binutils 后：

```sh
mkdir -p symbols
tar -C symbols -xzf symbols-linux-amd64.tar.gz
python3 scripts/symbolize-crash.py crash-123-456-789.json \
  --symbols symbols/app --symbols symbols/ffmpeg
# 也可以保留原始报告并输出附加解析字段的 JSON：
python3 scripts/symbolize-crash.py crash-123-456-789.json \
  --symbols symbols/app --symbols symbols/ffmpeg --format json --output resolved.json
```

只需传回轻量 JSON 报告。脚本根据每个模块的 GNU Build ID 查找 `.build-id/xx/rest.debug`，再核验 debug ELF 的 Build ID，通过 `addr2line -f -C -i` 输出函数、内联调用链、源码路径和行号；不联网获取符号，也不需要现网完整 core。报告的 `elf_address` 已去除加载偏移，不能再次减 load bias。脚本保留精确 PC，不统一做 PC-1：返回地址的行号可能落在调用后的语句，信号帧不适合盲目减一。源码路径来自构建时 DWARF，容器构建通常显示 `/src/app/...`。

缺少符号、模块 Build ID 或 ELF 地址时明确标记 unresolved 原因；发现归档文件 Build ID 不匹配直接失败，不按文件名猜测。`--require-module naki-live-worker` 可要求至少有 worker 的帧定位到源码行，否则非零退出。保留原报告中的超时、截断、缺线程/栈、unwind 错误标记：离线解析只能补充已捕获地址的符号，不能恢复被截断的帧、局部变量或原始内存。

自有包覆盖服务、worker、客户端、collector 及自定义 FFmpeg；发行版库的调试包需要另外按 Build ID 获取。`missing_symbols` 不表示这些模块没有崩溃；若故障发生在 libc/x264 内部，应补齐其匹配符号。可重复 `--symbols` 添加其他 Build ID 归档根目录。解析器不读取源码内容；拿到准确行号后，用 manifest 的源码提交检出对应文件查看。

本地自定义构建可手动导出（目标目录必须是新目录，输入必须还保留 DWARF）：

```sh
python3 scripts/archive-symbols.py --output out/symbols-unique/app \
  --source-revision "$(git rev-parse HEAD)" out/bin
python3 scripts/archive-symbols.py --output out/symbols-unique/ffmpeg \
  --source-revision "$(git rev-parse HEAD:experiments/decoder-20261016/ffmpeg-7.0.3)" \
  "${NAKI_BUILD_ROOT:-$HOME/.cache/naki-live-transcode}"/ffmpeg/lib*/*.so.*
```

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

如专项启用完整 core，应另行在专用测试实例验收“结构化摘要 → 可访问 core → Build ID 对应 → 能解析自定义 FFmpeg 的线程栈”。本轮验证的是轻量报告及其源码定位，未声称完成完整 core 的生成、存储和符号化验收。

参考：[core(5)](https://man7.org/linux/man-pages/man5/core.5.html)、[signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html)。
