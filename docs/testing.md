# 测试入口与基线

适用于 Linux/WSL 的实验服务。先通过 `bash scripts/build-service.sh` 构建当前源码；已有 SDK 可设置 `NAKI_SKIP_FFMPEG=1`，消费期望和字节校验仍执行。Python 要求 3.11+，编译任务数可用 FFMPEG_LIVE_BUILD_JOBS 设置。历史第四批源码窗口的 full 34 步、应用 sanitizer 6 步及两套各 86 场景见[核心整理验证](validation-core-refactor.md)；文末第一批、[日志](validation-logger-submit.md)和[生命周期](validation-pipeline-lifecycle.md)分别保留各自冻结窗口，不改写历史计数。

## 三个功能测试入口

```sh
bash scripts/test-service.sh quick
bash scripts/test-service.sh full --ffmpeg-symbols /path/to/ffmpeg-symbols --vvenc-symbols /path/to/vvenc-symbols
bash scripts/test-sanitizers.sh
```

`scripts/ci-test.sh` 保留为 full 的兼容入口。所有脚本从自身位置定位仓库；不要求当前目录是仓库根。`--list` 只打印所选清单、间接调用和独立门槛，不构建、不执行测试。

| 入口 | 实际覆盖 | 不代表什么 |
| --- | --- | --- |
| quick | 15 个带 quick 标签的原生测试；入口失败传播/超时/覆盖保护；SDK identity/expectation、发布绑定、事件维护检查 | 不跑服务媒体、真实网络、进程接管、完整崩溃或容量测量；并非纯静态 lint，部分检查会编译微型 ELF/创建临时 Git 仓库 |
| full | quick 加原 ci-test.sh 的全部 Python 套件，以及单/多输出生命周期矩阵：构建缓存、媒体/水印/SEI/音频/帧率/队列、真实传输、控制与接管、健康、finalize、崩溃、独立符号；保留原 VVC 单次测量 | 不包含实际 Docker smoke、外部采集、目标机器容量、24/72 小时 soak；单次 VVC 结果不是稳定性能结论 |
| sanitizers | 独立 ASan/UBSan 构建，15 个快速原生测试及 service、multi-output、watermark-review、lifecycle-media；默认启用 leak 检查及遇错停止 | 不重新插桩 FFmpeg、VVenC、系统 libevent/libcurl/libx264/libunwind；不证明整个媒体栈无泄漏/竞态，也不覆盖 TSan |

构建默认启用测试目标，可用 `NAKI_BUILD_TESTING=OFF` 只构建四个服务可执行文件；此时不能运行 native/完整 profile，但可用相应构建目录和 `--bin-dir` 运行不依赖 helper 的选定套件。

quick/full 需要普通 `NAKI_SANITIZERS=OFF` 构建。sanitizers 使用 Debug 构建和同一已验证 native SDK，将 CMake 缓存放在 `$NAKI_BUILD_ROOT/service-sanitizers`，二进制放入新的 `out/sanitizers-*/bin`，不替换普通 `out/bin`。普通构建仍默认 RelWithDebInfo（可用 `NAKI_BUILD_TYPE` 显式覆盖），编译告警门槛保持。既有 fatal handler 和故意非法写入探针的局部插桩边界保持原样；故意崩溃、preload 故障和离线符号矩阵仍在 full 中正常执行。

## 按套件运行

```sh
# 保留 full 的 SDK consumer gate，只运行这两套；不会顺带运行 native。
bash scripts/test-service.sh full --suite service --suite live-mux
bash scripts/test-service.sh quick --suite native
bash scripts/test-service.sh full --suite lifecycle-media --list
```

`--suite ID` 可重复，ID 必须属于所选 profile；未知、跨 profile 或重复选择均失败。执行顺序遵循清单，默认不带参数时仍执行完整 profile。`native` 表示全部 15 项 CTest quick；缺依赖和失败不会隐式跳过。结果中的 `scope=selected` 和 `selected_suites` 明确区分局部通过与完整回归通过。非 quick 选择始终执行 SDK gate；符号目录只在选择 symbols 时要求。

本次新增 organization 检查后，quick 是 7 个步骤，full 是 35 个步骤，sanitize 仍是 6 个步骤；历史记录中的旧计数不回写。独立性能入口见[性能测量](benchmarking.md)，不以性能测量替代功能门槛。

## 测试源码目录

| 目录 | 内容 |
| --- | --- |
| `tests/unit/` | C 原生模块测试 |
| `tests/integration/` | 文件、实流、控制、恢复、故障及符号集成驱动 |
| `tests/fixtures/` | 媒体生成器、独立探针、故障包装器 |
| `tests/support/` | 无导入副作用的 HTTP/启动/媒体/图片/进程工具；间接 budget 场景及 native wrapper |
| `tests/checks/` | 入口、模块边界、SDK、发布绑定和维护工具检查 |
| `tests/benchmarks/` | 独立媒体负载与 VVenC 时延测量、logger 微基准源码 |

公共启动器显式接收本套件结果目录，不借用另一个套件的 RUN；端口、目录和进程只在运行时创建。Python 场景仍各自拥有服务停止、故障解除和样本语义。原路径迁移表保存在本次整理证据中；专题脚本可通过新路径直接执行。

## 依赖与选择参数

- quick 需要 Linux/WSL、Python 3.11+、Bash、CMake/CTest、C 编译器、Git、readelf/objcopy/strip，以及已构建的服务/原生测试。Python 断言不能关闭。
- full 另需 make、GCC、pkg-config、系统 ffmpeg/ffprobe（用于生成与独立检查样本）、OpenSSL CLI、addr2line/nm、带 CLI 的匹配 native SDK、匹配的 FFmpeg/VVenC DWARF。传输服务和端口均为测试本地资源；音频签名套件会下载带校验的 FFmpeg FATE HE-AAC fixture，需要相应 HTTPS 网络。
- 已有 HE-AAC 样本可通过 `NAKI_HE_AAC_FIXTURE` 提供，套件仍核对固定大小和 SHA-256；不能用未校验的替代样本让检查通过。
- 默认 SDK/CMake 路径遵循 `NAKI_BUILD_ROOT`，缺省为 `$XDG_CACHE_HOME/naki-live-transcode-n9` 或 `$HOME/.cache/naki-live-transcode-n9`。`--build-dir`/`NAKI_SERVICE_BUILD_DIR` 可指定应用 CMake 目录。
- `--bin-dir`/`NAKI_SERVICE_BIN` 指定可执行目录；构建入口也支持这两个目录变量，应使用绝对路径。quick/full/sanitize 均将该路径传给相应驱动；原生测试还会核对 CTest 注册的可执行目录，拒绝测错构建。
- 测试镜像通过独立的 `/src/expected-sdk.json` 继续执行 SDK gate；三份 decoder probe 源码单独放在 `tests/decoder-probes`，由 `NAKI_DECODER_PROBE_SOURCE` 指定，不伪装成不完整的 FFmpeg checkout。源码完整的本地环境继续使用子模块原路径。
- full 的 native 符号默认 `/symbols/ffmpeg`、`/symbols/vvenc`，可用上述参数或 `NAKI_FFMPEG_SYMBOLS`/`NAKI_VVENC_SYMBOLS` 指定。sanitizers 不要求这些符号目录。

所选入口会先检查必要工具、测试源、二进制/helper、CTest 注册与目录是否对应，完整/消毒器入口还执行 SDK consumer gate。缺依赖是失败，不隐式跳过。测试串行运行，避免多套实流同时争抢资源。缺少冻结历史媒体不影响服务 full；它只影响独立的 `scripts/test-decoder.sh`。

`bash scripts/test-service.sh full --suite decoder` 除既有生命周期/预算测试外，还构造14种入流重排结构：HEVC 3B/7B × closed/open GOP × B-pyramid关/开，另含7B的10-bit及H.264 3B/7B closed/open。实际核对连续B长度、参考B、CRA/RASL及IDR数量；2/12线程、capacity=1、1/3轮回放逐帧比较像素、PTS、显示顺序和元数据，并用系统FFmpeg提供独立像素SHA256对照。HEVC允许输出触发包的 `pkt_dts` 改变，H.264仍要求该字段也一致。这是小样本正确性回归，不是不同入流结构的性能基准。最近一次证据见[入流重排矩阵](validation-input-reorder-20261022.md)。

## 清单和间接调用

机器可读清单为 [service_suites.json](../tests/service_suites.json)。入口保护测试保存旧 ci-test.sh 的 Python 清单，确保迁移后每套仍执行一次，并核对 Docker build 阶段包含所有 CMake 源文件及其本地头文件，避免后续 test 阶段的完整拷贝掩盖漏项。CTest 的 quick 标签必须与清单完全相符。

| 测试/工具 | 执行位置 |
| --- | --- |
| 13 个无参数原生测试、event-logger 和 logger-submit | CTest quick；两个日志 wrapper 每次建立新目录，logger-submit 覆盖准备期间的保序、限额、释放/停机与维护 |
| logger | observability_integration；CTest 另提供独立 wrapper，未放进 quick 以免 full 重复执行 |
| live-mux 原生检查 | live_mux_integration；CTest 另注册为 integration-helper |
| lifecycle-media | full/sanitizers 均执行旧单输出、outputs 单元素、2/4 路的 86 场景矩阵；仅测试 worker 链接故障包装器，覆盖部分初始化、停止、错误优先级及资源归还 |
| watermark-render | watermark_integration / watermark_review_integration |
| watermark-state / watermark-operation | watermark_review_integration，使用其生成的资源 |
| probe-budget | probe_integration → probe_budget_integration |
| live-input-poll / service | live_input_stability_integration → 两个 live_input 子套件，使用共享新 fixture |
| output-video-probe、SEI/audio/transport fixture、hook worker、fault shared objects | 清单中相应 Python 套件，属于样本/检查/故障工具，不作为无参数成功测试注册 |
| workflow_publication_test | dependency workflow 在 SDK 查询前独立执行；只运行本地 stub，不发布 |
| docker_smoke | Service workflow 在 full/sanitizers 后，用实际运行镜像与匹配符号独立执行 |

直接 `ctest --test-dir <build>` 目前执行 17 个原生测试；`ctest -L quick` 执行其中 15 个。两者均不能代替 full。原来的专题 Python 脚本仍可独立运行。

## 证据与完成标准

每次运行创建唯一的 `out/checks-<profile>-*/`，包含 `summary.json`、前后源码哈希清单和每个步骤的命令、日志、退出码、耗时、测试期间新增的结果目录。可以用 `--result-dir out/<new-name>` 指定尚不存在的目录；拒绝覆盖旧结果或写到 out 之外。

源码摘要覆盖 app/scripts/tests/.github、构建配置及 yyjson 实际源码；app 目录中的 README.md 也在内，docs/ 和根 README 不在该摘要中。它独立于 Git dirty 状态，在无 .git 的测试镜像内也能生成，并记录提交、所选 ELF/helper 哈希和 SDK receipt identity/哈希。它是测试输入身份，不是 release manifest，也不单独证明二进制来自该源码；基线须附构建记录。native 源码/安装字节由 SDK consumer gate 检查。第五批更新模块 README 会改变总摘要，应保留旧测量身份，并以逐文件非文档哈希证明业务实现未变，不冒充新媒体回归。

只有所有所选步骤退出 0，前后源码、所选二进制及 SDK manifest 均未变化，summary 才标记 passed。任何预检失败、子测试失败或超时都返回非零；失败日志保留。测试入口保护用真实失败的 CTest、缺失 executable 和超时子进程验证这些边界。

原水印复审保留可选的可信 HTTPS 成功样本：设置 `NAKI_TEST_HTTPS_URL` 才执行，未配置时 COMPLETE.json 的 `not_run` 会提取到顶层 summary；套件通过不代表这项外部路径已验证。非可信证书及重定向的本地 TLS 拒绝路径通过所需 OpenSSL CLI 执行，不再因缺工具而静默省略。

基线只宣称当前候选的功能验证。服务 ingress-to-mux、客户端上传、显示时延、socket 时间仍按各自既有边界解释；未执行的镜像/采集/宿主机验收不写成通过。旧实验和 n7.0.3 冻结证据不变。

## 第一批本机基线（2026-10-18）

环境为 WSL Ubuntu、Python 3.12.3、CMake/CTest 3.28.3、GCC 13.3.0；应用构建最多 2 个任务。基于主仓 `a1b7e0584355756d0c5b13eaa6668d2a724d2a45` 加本批未提交的测试/构建/文档修改。测试输入 SHA-256 为 `061ad959d7d1b91ba557634eedea92608642bc44017860c6e27f707746319980`，按上文定义覆盖 app/脚本/测试/构建配置；不包含根 README 和 docs，但包含 app/README.md。

复用 SDK identity `4c6d30a2595d52765a93db3b68ad2a5a0dfca6e6f1880d1a0607041f536a4e16`；FFmpeg pin `4bee779b3dd59407a07baa8d6fc386cf95d8398b`、VVenC pin `29e3dc57a91d03833ead6370f5cd03f6d61242a5`。消费 gate 与既有 SDK 字节校验通过，未重建或修改实际 native 库。SDK 回归自行编译的微型 fixture 位于本轮 out 子目录内。

本轮本机证据根为 `out/batch1-baseline-516a5160/`，生成物不入 Git。

| 检查 | 实际结果与证据 |
| --- | --- |
| 当前应用构建 | 通过，`candidate/build.log` |
| quick | 6 个步骤通过，包括 14 个原生测试及 5 套 Python 检查；`candidate/quick/summary.json` |
| full | 33 个步骤全部通过，保留旧 ci-test.sh 全部 Python 套件；`candidate/full/summary.json` |
| ASan/UBSan | 5 个步骤通过，包含 14 个原生测试和 3 套媒体集成；`out/checks-sanitize-fi8u6b9u/summary.json`，Debug 构建日志位于 `out/sanitizers-MZBcYQ/build.log` |
| 原生独立入口 | logger/live-mux 两个 helper 通过；`candidate/ctest-helpers.log` |
| 轮转边界回归 | event-logger 连续 8 次通过；`candidate/event-logger-boundaries.log` |
| SDK 发布策略 | 真实 workflow shell 配合本地 stub 的两项测试通过，没有实际发布；`candidate/workflow-publication.log` |
| 镜像 probe 复制路径 | 复制后的三份源码字节一致，预检、入口测试及 H.264/HEVC 解码回归通过；`probe-copy/summary.json`。这是路径验证，不是实际 Docker 执行 |
| 静态与范围检查 | YAML 重复键、shell 语法、JSON、文档链接及 app/native 对 HEAD 未改动检查通过；`static-final.json`，版本与身份见 `environment-final.json` |

quick、full、最终 sanitizer 的测试源码摘要相同；各自前后源码、所选二进制和 SDK manifest 均未变化。普通构建及其 quick/full 的组合证据为 `candidate/summary.json`；聚合索引为 `baseline.json`。

整理过程中修复了两类测试问题：新输出/构建变量与独立 SDK 期望不再泄漏进微型 fixture；事件可见性检查同时接受 active 或已正常 sealed 的文件，保留原 750ms 期限和队列仍繁忙的判据。应用日志实现没有改变。最初的优化 sanitizer 构建遇到编译告警升级错误，最终独立入口使用 Debug，未降低告警门槛或关闭运行时检查。

失败尝试保留在 `out/checks-full-bhdtxi_q/`、本证据根的 `quick/` 以及 `out/sanitizers-3WgF90/`；不将失败尝试覆盖成成功结果。

仍未执行：未配置 `NAKI_TEST_HTTPS_URL` 的可信 HTTPS 水印成功子项（full/sanitizer 各自显式记录 not_run）；实际 Docker 镜像构建/smoke（本机 daemon 未运行）；远程 GitHub Actions；外部采集、目标机容量/长时稳定性及发布部署。本轮不据此宣称生产就绪或性能改善。
