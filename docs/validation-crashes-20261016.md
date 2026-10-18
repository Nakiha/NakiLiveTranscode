# 0.4.0 轻量崩溃采集验证

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

日期：2026-10-16（Asia/Hong_Kong）。环境：WSL Ubuntu 24.04 x86_64、GCC、系统 libunwind 1.6.2；Docker Desktop Linux engine，镜像 Ubuntu 24.04，以 UID/GID 10001 运行。FFmpeg 与 yyjson 的已固定子模块没有改变，未重跑冻结解码器基准，未修改宿主机 core_pattern。

## 实现边界

HTTP 和每个 worker 各预启动独立 C collector。handler 只发送 384 字节固定现场并执行有期限的等待/终止操作，不展开栈、不调用 yyjson/logger/allocator/stdio/应用锁。collector 临时派生隔离抓栈子进程，以原始故障寄存器展开；JSON 和限额文件 I/O 在 collector 内完成。默认 2 秒、单份 256 KiB、32 份/16 MiB，目标退出前抑制完整内核 core。监管入口依据用户假设不纳入崩溃故障注入。

## 检查与本机证据

| 检查 | 结果 | 新生成证据目录（不上传） |
| --- | --- | --- |
| 真实非法地址写入 | 通过：SIGSEGV 的 fault_address=0x1、fault_pc 与首帧 PC 一致；命名寄存器 rip/rsp 匹配原始 PC/SP；恢复三层原始调用链 | `out/service-test-iel15kk4` |
| 符号对应 | 通过：模块 Build ID、load bias/elf_address，addr2line 解析到 crash_fixture_level3 和源码行 | 同上，`symbolized.txt` |
| 非主线程与异常现场 | 通过：独立 pthread 崩溃、其他线程持有应用 mutex/stdio 锁、主线程备用栈承接栈溢出、双线程同时故障 | 同上 |
| 大小和保留 | 通过：线程/帧/远程读取预算，4 KiB 单份上限，2 份/8 KiB 目录预算；JSON 完整且无重复键；保留 symlink/无关文件，清理遗留 tmp；4 个进程并发发布 | 同上 |
| 抓栈进程卡死 | 通过：测试 interposer 使 ptrace 读取永久等待，外部 watchdog 杀掉抓栈子进程、释放 tracee，生成 capture_timeout 固定现场回退 | 同上 |
| 权限与子进程失败 | 通过：ptrace 被拒绝时保存寄存器并明确 complete=false/EPERM；抓栈子进程自身故障时保留 capture_process_failed 报告 | 同上 |
| collector 失联 | 通过：杀死 collector 时目标仍退出；暂停 collector 后目标按 200 ms handler 期限退出，collector 恢复后只记录 target_already_exited | 同上 |
| worker SIGSEGV | 通过：独立报告含 worker/job_id/线程栈，控制 API 继续响应，父进程确认 SIGSEGV 且 core_dumped=false | 同上 |
| HTTP SIGSEGV | 通过：独立报告含 http/线程栈，原 UDP 转码继续写出；重启同配置控制进程后接管同 PID worker，停止和输出独立解码通过 | 同上 |
| ASan/UBSan 崩溃矩阵 | 通过，包含上述真实故障、限额、失败回退和角色接管 | `out/service-test-l7gqox0p` |
| 原完整链路 | 通过：文件/HEVC/B 帧、缩放、UDP 输入输出、错误处理、取消、SIGTERM | `out/service-test-2iwhxphk` |
| 原观测矩阵 | 通过：非重叠窗口、I 帧、TCP/HTTP、UDP 后台与直接路径、阻塞写出、日志队列/轮转/清理和默认 10 秒摘要 | `out/service-test-334i6z02` |
| 待命池/控制接管矩阵 | 通过：待命补位、崩溃隔离、心跳/停止期限、失联媒体继续、离线完成和冷/暖启动功能检查 | `out/service-test-eug2o2gn` |
| ASan/UBSan 原完整链路 | 通过，包含正常退出时的 leak 检查 | `out/service-test-2v5ysnj8` |
| 非 root Docker | 通过：worker/HTTP 的 SIGSEGV 都有实际线程栈；没有添加 SYS_PTRACE capability 或关闭 seccomp；HTTP 由监管入口重启并接管同 worker，75 帧输出完整，容器 RestartCount=0；健康检查、维护卷和 SIGTERM exit=0 通过 | `out/docker-smoke-4nrp6moi` |

ASan 故障矩阵使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:handle_segv=0:verify_asan_link_order=0` 和 `UBSAN_OPTIONS=halt_on_error=1`。handle_segv=0 让本程序 handler 接收故障；verify_asan_link_order=0 允许测试专用 LD_PRELOAD interposer。handler 和有意非法写入的极小探针关闭 sanitizer 插桩；其余应用/collector 仍插桩。系统 libunwind、自定义 FFmpeg 及所有系统依赖没有全部插桩。

对 RelWithDebInfo 的 fatal_signal 反汇编检查，调用只有 errno/getpid、Linux syscall、clock_gettime、send、poll、sigaction、sigprocmask 和 _exit；lock-free guard 内联，没有 yyjson、logger、malloc/free、stdio 或回溯库调用。

## 本次报告体积

这里只说明当前测试实例，不能推导所有线上线程/模块组合的大小或完整程度。触发方式为真实角色进程收到 SIGSEGV；原始 SIGSEGV 内存访问与源码定位另由 fixture 检查。

| 环境/角色 | 抓到线程 | JSON 文件字节数 | 结果 |
| --- | --- | --- | --- |
| 原生 HTTP | 2 | 4203 | captured，complete=true |
| 原生 worker | 4 | 6462 | captured，complete=true |
| 容器 HTTP | 2 | 4187 | captured，complete=true |
| 容器 worker | 4 | 6426 | captured，complete=true |

本次镜像 ID：`sha256:338896ec84d70ac5ea45a016d0aef2a15fdc5d1c696746c0c81c2855ae71862b`。Docker 测试使用唯一 Compose 项目和新建卷，结束只清理该测试资源，保留镜像。报告原件/测试导出在各证据目录；没有将现场地址或二进制 dump 提交公共仓库。

## 未验收与限制

- 当前运行验证仅 x86_64；AArch64 分支未做实机验收。未声明所有发行版、Yama/LSM/seccomp 策略都允许抓栈。
- 主线程和本应用线程有备用信号栈；不能保证未注册备用栈的 FFmpeg/第三方线程发生栈溢出时还能送出现场。
- SIGKILL/OOM/主机或容器消失不经致命信号采集链；collector 不可用、磁盘错误、超过限制、栈损坏可能只留下摘要或固定寄存器。
- 没有导出完整堆/帧缓存/原始栈内存和局部变量，因此不能代替所有内存损坏根因分析。保留匹配 Build ID 的符号文件仍是发布要求。
- 内核不可中断 I/O 的极端情形不能保证严格墙钟期限；代码有有界回收及按 pidfd 终止已崩溃目标的回退，但没有注入真实内核 D 状态验收。
- 未验收完整 core 链路，也没有修改全局 core 收集设置。未对简单监管入口做崩溃注入、海量并发崩溃或长期 soak test；没有新增生产时延/容量承诺。

运行与配置见 [轻量崩溃采集](crashes.md)。
