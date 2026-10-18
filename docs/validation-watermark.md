# 单水印 v1 本地验证（2026-10-17）

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

当前状态：前轮 3 项 P2 与预算加固已关闭；本轮另修复首帧前关闭水印的时序问题。最新证据为 `out/watermark-boundary-20261017T1410/`：33 项状态矩阵及 23 项本地复审测试通过，原 30 项验收和其余功能回归通过。公共可信 HTTPS fixture 本轮加载失败，独立 curl 重试连接超时，明确保留为未完成的外部验证；前轮 HTTPS 通过仅作历史证据。最终独立源码复审通过，无新源码阻塞；本轮外部 HTTPS 成功路径仍未完成。下文前轮 `watermark-p2-acceptance-20261017T1250/` 及 `watermark-final-20261017T1220/` 均为保留的历史窗口。

本批已实现并通过本地功能验证，尚待独立源码复审。父仓仍为 `4ebf6eed7d53c241d92f7b9e8f22622ed76d70fc`，分支 `codex/buffered-live-probe`；水印改动未提交、推送或部署。FFmpeg `d8303fa436059a96bb773df8566a73c6253c8159`、VVenC `29e3dc57a91d03833ead6370f5cd03f6d61242a5` 的源码和 gitlink 未改变。

用户确认的合同是单水印根配置、带 revision 的 `POST /v1/job/watermark` 完整替换、required 起播失败则任务失败、热更失败保留旧资源、required 不能运行中关闭，以及本地文件和 HTTP/HTTPS 资源。完整边界见 [单水印 v1](watermark.md)；通用 v2 管线草案没有随本批开放。

| 需求 | 代码入口 | 本批证据 |
| --- | --- | --- |
| JPEG/PNG/GIF 与透明度 | `app/watermark.c` 的 load/prepare、独立资源线程 | 8/10-bit 未编码像素、PNG alpha、GIF disposal 2/3、实际可变时长和循环边界 |
| URI、锚点、偏移、尺寸热更 | `app/watermark_config.c`、`app/service.c`、`app/worker.c`、帧边界切换 | 独立解码比较热更前后位置和尺寸；同 URI 改内容后 SHA-256 与蓝色像素更新；连续尺寸请求合并 |
| required 首帧门槛 | `naki_watermark_wait`、`pipeline_main` | 延迟资源等待期间无输出；门槛后首帧前更新失败仍使用初始资源；累计预算不因 revision 重置；超时、取消和无效资源 |
| 失败和并发版本 | worker 持有 accepted revision，独立 ACK token | 旧 revision 409、accepted/applied 区分、坏图保留旧 revision/像素、可选水印从无到有及关闭 |
| GIF/内存边界 | 固定 8MiB 编码、2097152 源像素、128 帧、60 秒、每代 64MiB RGBA 上限 | 实测拒绝编码超限、GIF 帧数/时长/解码后字节超限及零时长多帧；像素上限与 PNG 压缩元数据拒绝还经源码检查 |
| 颜色与主视频精度 | 固定独立 overlay 图、显式 logo BT.601/709 矩阵与范围、alpha 预乘缩放 | BT.601/709、full/limited 的 8/10-bit 像素；中途 HDR 标记显式失败；opaque_ref、PTS 和像素格式保持 |
| VVC/B 帧与真实写出追踪 | `pipeline.c` 的 opaque_ref/PTS ledger/逐帧 watermark_revision | 125 帧 10-bit 水印输出独立解码为 3 I、32 P、90 B；125 个身份和 revision 完整匹配 |
| 进程恢复和取消 | worker 私有控制通道、独立 loader 生命周期 | 控制进程死亡后同 PID worker 恢复并继续更新；加载中停止、旧候选取消；既有崩溃/进程回归通过 |

最终证据目录是 `out/watermark-final-20261017T1220/`：`ACCEPTANCE.json` 汇总实际 exit code、源码摘要和证据路径；`source-snapshot/` 保存全部改动文本，包括未跟踪的新文件；`parent-working.patch` 是无 textconv 的 binary-safe 父仓差异。源码、测试及配方在最终构建和回归窗口保持一致；结束时仅文档继续补充结果，差异单独记录。

最终 SDK identity 为 `952134bb13b47fe2de5099abebc8e156b0f35c9c67d395fdce05989517f084e1`。用户明确允许后只安装了缺失的 `zlib1g-dev`，版本 `1:1.3.dfsg-3.1ubuntu2.2`；头文件/链接探针和新 PNG SDK 构建成功，未升级其他包。Docker 构建/运行配方也声明了 zlib。

最终运行了水印 30 项验收、frame_policy/media_hooks/probe/decoder/service/vvenc/vvenc_pts/live_mux/process/crash 共 11 套 Python 功能回归、3 个 C 测试程序和 12 项 SDK 身份单测，全部通过。`vvc-watermark-proof.json` 保存实际 VVC 帧类型。匹配的 17 个应用 ELF 的独立 DWARF 位于最终目录 `symbols/application/`；同 SDK 的 6 个 FFmpeg、1 个 VVenC 模块复用 `out/watermark-review-20261017T1210/symbols/`，不是旧 n7 性能证据。符号测试实际覆盖 PIE fault、strip 后解析、源行、错误 Build ID，以及 FFmpeg/VVenC 库函数 DWARF。

早期原始失败记录没有覆盖：测试脚本的 Python 命名/字段错误已修正；严格构建发现的枚举比较告警已修正。崩溃测试最初使用 Windows 挂载目录，实际权限为 0777，collector 正确拒绝要求的 0700；符号测试还曾误用默认 Docker `/opt/naki-build` 路径。最终使用原生 WSL 缓存临时目录、已有 `--state-dir` 参数和显式 `NAKI_BUILD_ROOT` 重跑通过，没有修改系统设置或削弱权限校验。旧启动器早期还沿用了服务默认 `/tmp` 私有状态目录，这是当批写入范围的偏差；最终启动器的私有端点/报告全部限定在现有 WSL 缓存，旧记录保留供审计。

本批没有运行 Docker 镜像构建、远程 CI、ASan/TSan 或性能/容量基准。三槽全画布合成的内存带宽开销仍需单独测量；每代 RGBA 上限不是 worker 总 RSS 上限。热更中的资源阶段没有独立公开下载/解码/缩放直方图。HDR/BT.2020、ICC、APNG、多水印、百分比布局和通用在线规格重建均未实现；GIF 循环/零时长等首版策略是显式固定边界。不能据此称为生产就绪，或声称已完成所有后续复杂需求。

## 独立审查修复与重新验收

| 审查项 | 修复 | 验证 |
| --- | --- | --- |
| 首帧前可选启用遇到不兼容视频会终止任务 | `app/watermark.c` 单独保存初始启用状态和视频实参是否已知；不兼容时撤销可选更新并取消准备，loader 发布时再次检查，初始无水印配置保持有效 | `tests/watermark_review_integration.py` 的可控 TCP 输入先连接但不交付视频；真实 HTTP 更新后分别等待候选准备或保持加载中，再交付 HDR；超限画布实际编码 4096×2162 五帧。三种更新任务完成且像素无水印，两种初始启用对照仍在输出头前失败 |
| PNG cHRM 的 BT.2020/未知原色被当作普通 RGB | cHRM 使用固定 SDK 的原色分类和容差，只接受 BT.709；解码后也仅允许 BT.709 或未标记原色 | 标准及舍入 BT.709 PNG 成功；合法 BT.2020、未知原色 PNG 拒绝；required 热更失败后原红色水印与 applied revision 保持 |
| overlay 后再次复制属性追加 side data | 直接移动 overlay 返回的主帧，不再调用第二次 `av_frame_copy_props` | 先复现 3 条 side data 变成 6 条；修复后每帧保持 3 条，逐项验证 A53 字幕、unregistered SEI、S12M timecode 的数量与字节内容，并验证 metadata/opaque_ref/PTS |
| 超限帧先分配后拒绝 | 先用公开布局 API 计算对齐的 RGBA 像素字节并检查剩余预算，再分配目标帧；分配后只核对布局一致性 | 测试专用链接包装统计真实 `av_frame_get_buffer` 调用：2048×2048 GIF 在预算拒绝前的大 RGBA 分配次数从 5 降至 4，不额外申请第五帧 |

补充网络覆盖包括三次重定向成功、第四次/循环/转到 file 协议拒绝、跨跳转总超时；本地自签名 HTTPS 及 HTTP 跳转到该 HTTPS 都因证书校验而拒绝，TLS 服务未收到 HTTP 请求。用默认 libcurl TLS 校验成功加载 `https://httpbin.org/image/png`。复用已有 OpenSSL 生成本地测试证书，没有安装依赖、导入 CA 或关闭验证。CI 未配置可信 HTTPS fixture 时会明确记为未运行，不伪报成功。

ACK 测试通过 pidfd 暂停并核对本测试服务的 worker 父进程：分别在 ACK 前、2 秒 ACK 超时前断开并复位客户端连接。超时后再提交同 revision，旧 202 ACK 不能误答新请求，新请求必须得到 409；随后正常更新和停止仍有效。

最终源码重新构建，新增 19 项审查回归、原 30 项水印验收、共 12 套 Python 功能回归、3 个 C 测试程序、12 项 SDK 身份单测及符号测试全部通过。新增测试接入原 `scripts/ci-test.sh`，没有增加 workflow。新应用 17 个 ELF 的 DWARF 位于本窗口 `symbols/application/`；SDK 源码/配方不变，identity 仍为 `952134bb13b47fe2de5099abebc8e156b0f35c9c67d395fdce05989517f084e1`，FFmpeg/VVenC 独立 DWARF 继续引用匹配的 6+1 模块归档。

`ACCEPTANCE.json`、27 文件快照、SHA-256 摘要、精确 git 状态和 binary-safe 差异位于最新证据目录。修复前的 side-data/预算失败及修复后直接观测保留在 `out/watermark-review-fixes-20261017T1300/`，未覆盖初版证据。父仓 HEAD 及子模块 pin 不变，没有提交或推送；没有审批或执行器阻塞，尚待本次修复的独立复审。

## 首帧前关闭的时序修复

本轮证据目录为 `out/watermark-boundary-20261017T1410/`。旧代码的固定 `initial_enabled` 会使“初始开启、required=false、首帧前 revision 2 关闭已经 prepared、随后 HDR 或超限画布首帧”仍失败。确定性 C 测试在修复前 33 例中复现 2 例失败，见 `before-state.json`；未覆盖或替换旧证据。

现改为完整的帧边界有效配置：初始配置作为兼容性基准，准备好的关闭先在首帧边界生效；失败/未就绪的启用候选保留基准。关闭不读取资源，接受时同步 prepared，取消旧加载；applied 仍必须等视频帧。required 的关闭请求在接受前拒绝。关闭时主帧直接透传，旧合成图不再影响其色彩兼容性。

| 初始配置 | 候选 | SDR 首帧 | HDR / 超限首帧 |
| --- | --- | --- | --- |
| 关闭 | 无 | 透传 revision 1 | 透传 revision 1 |
| 关闭 | 启用已准备 | 新图 revision 2 | 更新失败，保留透传 revision 1 |
| 关闭 | 启用失败 / 未就绪 | 透传 revision 1 | 保留透传 revision 1 |
| 开启 | 无 | 初图 revision 1 | 任务失败 |
| 开启 | 启用已准备 | 新图 revision 2 | 任务失败 |
| 开启 | 启用失败 / 未就绪 | 未应用图像 revision 0 | 任务失败 |
| 任一可选初态 | 关闭已准备 | 透传 revision 2 | 透传 revision 2 |

关闭候选的“资源准备失败 / 仍在准备”不再是可达状态；它无资源 I/O。非法配置和 required 关闭在接受前拒绝，不生成候选。测试通过链接器包装仅拦截测试程序的 loader 启动，确定性覆盖两种初态、五种候选状态、三种视频属性共 30 例，另有 3 例 required 控制。真实 HTTP/TCP 测试增加已加载和正在下载初始资源时的首帧前关闭，分别覆盖 HDR 和 4096×2162 输出；检查同一 worker、完成 5 包、applied revision 2 及无水印像素。

最终源码复审于本地提交前完成，无新源码阻塞。首帧关闭修复的完整证据仍为上述冻结目录；提交前仅更新本段及项目状态措辞，未重写历史测试结果。当前 23 项本地复审通过、1 项外部 HTTPS 成功路径未完成，不能将本轮描述为全绿。
