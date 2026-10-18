# 第二批帧策略：本地功能验收与复审修复

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

当前实现仍未提交、未更新父 gitlink，也未发布；等待修复后的独立源码复审，水印保持暂停。此前 WSL 审批阻塞已经通过用户直接授权及正式按动作审批解除。

## 最新修复与证据（2026-10-17）

匹配媒体 SDK 为 `78da00f3106213fe4da08eb14d13aae83c9901836b200e0711e23dbde612bf5a`，首次完整构建与 skip 见 `out/frame-policy-resume-20261017T1055/`，FFmpeg 6 个、VVenC 1 个以及当时应用 15 个 ELF 的匹配 Build ID 符号均已独立归档。本轮 P2 只改 app/测试及文档，媒体库代码和配方未改；新应用由标准 `NAKI_SKIP_FFMPEG=1 FFMPEG_LIVE_BUILD_JOBS=2 scripts/build-service.sh` 验证同一 SDK 后重建。

独立复审的 4 项 P2 均已用修复前代码/应用复现，原始观测在 `out/frame-policy-review-p2-20261017T1120/before.json` 与 `deadline-before.json`：

- 8 张源画面 PTS 严格递增，FLV 毫秒量化却重复，旧任务仍 completed。现在在保留画面送编码前按最终容器精度/显示顺序检查，接受后推进；碰撞明确失败，不补 tick，不要求编码序 packet PTS 单调。
- source_all + VVenC reference 旧配置被接受，120 帧 VFR 样本最大 PTS 偏差约 4.334ms。现在无论是否提供 intra 都在 HTTP 阶段拒绝，legacy + reference 仍有成功回归。
- 20ms 进入降级、120ms 截止、140ms 才完成恢复，旧实现错误清回 normal。现在比较恢复完成与耗尽的计划时刻，恢复必须严格早于截止；等时耗尽优先，也验证了较早恢复但轮询迟到的情况。
- recover_below_ms=0 旧配置被接受，年龄严格小于零永远不可达。现在解析器与 OpenAPI 都要求正值，配置单测及 HTTP 组合测试明确拒绝零。

媒体 hook 原测试的旧 `known_types=0` 预期已经修正。`tests/media_hooks_integration.py` 保留全部 10 个生命周期/拒绝/取消场景，并对 x264/TS、VVC/TS 与 VVC/MP4 每帧 I/P/B 做独立解码对照。帧策略测试也逐帧对照类型、按每个实际 I 边界验证完整/短 GOP4 组；过载测试从原始输入解码独立得到全部源 I 和 300ms 到期 I 的 frame_id，逐一证明留在输出且实际编码为 I，保存 `overload-recovery-protection.json`。

最终回归已完成：`out/frame-policy-review-p2-20261017T1120/final-run.log` / `final-run.exit=0`。两项核心/配置 C 单测、Python AST/OpenAPI JSON 检查通过；帧策略 `out/frame-policy-34g743nt/COMPLETE.json` 为 24 场景通过，原媒体 hook 的 10 场景及 probe/decoder/service/VVenC/VVenC PTS/live_mux/process 共 8 套默认回归均在最终应用上重新通过，不使用诊断副本替代原测试。对应单套日志/退出码均以 `final-` 开头；旧失败日志和 out-only 诊断保持原样。

此次保护帧受控样本中，独立输入解码得到 17 张源 I 和额外 33 张到期 I，实际输出 50 张 I 与这两组完全一致；277 张保留、43 张丢弃的 frame_id 合起来恰为原 320 张，保护集合无丢失。以上仅为该夹具事实，不是容量或丢帧率承诺。最终源码完整清单/快照、三个仓库二进制安全补丁、SDK 清单、当前应用 Build ID 符号及汇总分别归档在同轮 `changed-files.txt`、`changes.json`、`source-snapshot/`、`*-working.patch`、`sdk-manifest.json`、`symbols/application/` 和 `RESULTS.json`；匹配媒体库符号复用上述构建轮的独立归档。

修复新夹具时还发现，清理 recovery SEI 的 copy 步骤曾丢失非零时间起点；之前名为 offset 的源实际上从 0 开始，不构成非零起点证据。现在保留 copyts 并断言首 PTS=5s，再验证 MP4 绝对 PTS 和严格 FLV 的合法 B 重排。`flv-diagnostic.json` 保留旧夹具首 PTS=0、首 DTS=-80ms 被正确拒绝的诊断。

这些是本地功能验证，不是生产时延/容量、任意长时钟、多次回绕或高规格长期稳定性证明。source_i_anchors、脏时间戳修复、SEI/SCTE-35、多输出、在线视频规格重建仍未实现。后续历史段落中的“未运行”“审批拒绝”仅描述当时过程，不是当前权限或验收状态。

## 历史：第一批独立基础

第一批 SDK 对象/配置凭据失败安全修复已经实测通过：`out/sdk-recovery-acceptance-20261017T095859244/ACCEPTANCE.json`，SDK `31772e67658363b9d03e17c1b04d9b6aa63355c3772dd9a7ccd669bd2767651b`。包括 12 项单测、全部成功/失败/信号/封存恢复夹具及真实默认构建和 skip。这是第一批证据，不替第二批背书。

## 实现范围

- 独立 `frame_policy.c/h`：严格显示时间戳、可识别 modulo wrap、源空洞、I 截止与接受后推进、因果选择、过载滞回/恢复/连续耗尽、有界 512 项在途身份。
- `frame_policy_config.c`：新增可选 timestamps / intra / frame_selection 组，缺省保留 legacy；完整显式阈值才启用过载，重复/未知键及不支持的 repair/refresh 合同拒绝。
- 管线与 worker：独立 frame_id / generation、包/帧引用生命周期交接、实际 mux/flush 退休、输出 I 类型确认、50ms 独立监视和控制器断开后的过载失败路径。音频 mux 所有权、单路限制和既有时延分类不改。
- 匹配 FFmpeg / VVenC fork 的普通 I 请求、PTS-group 边界与实际类型统计；新增 API 宏 1，libavcodec 微版本 63.4.103。没有提交、推送或更新父 gitlink。

## 历史：初版失败与阻塞过程

`out/frame-policy-implementation-20261017T101525862/build.log` 的初版完整构建完成，SDK 为 `41a9a9e6c8ca6e8bc6012840c3c4b780329f2b7b7152e3f0fd344a39c46ef65f`；纯策略与配置单测通过。它仍有下面实际码流问题，不能视为功能验收通过。

1. `integration.log`：首版夹具把 H.264 decoder key 标志当作 IDR。实际另存诊断样本证明移除 recovery SEI 后，普通 I 的 key 标志从 1 变 0；已改为检查实际 NAL 类型，并生成非 key I 源画面。
2. `integration-BP8vU8.log`：精简 fork 未启用 settb/setpts，现使用既有系统 FFmpeg 生成时间夹具，再由 fork 编码；没有扩展产品构建依赖。
3. `integration-yB6fid.log` 与 `out/frame-policy-ysffzqql/plain-libx264-follow_source.mp4`：实际 NAL 检查发现无 B 帧 x264 把所有请求 I 提升为 IDR。新源码已经设置较大的 min-keyint 和有限会话硬出口，保持无 B 帧且不允许静默提升；修复未完成运行验证。
4. 修正后的前台重建被执行服务器连接中断，`rebuild-ordinary-i.log` 记录 Make 收到 Hangup。这是执行器传输故障，不是编译诊断或审批拒绝。

最新源码还补了严格输入 NOFILLIN，以及 HEVC 10-bit / 分数帧率 VFR、PES 重复/回退/缺失/回绕、严格 FLV 预滚范围和阻塞 TCP 过载夹具；这些新增路径尚未通过运行验收。VVenC 普通 I 及全部默认媒体回归也尚未通过第二批验收。不能使用现有二进制宣称这些代码已构建。

## 历史：此前审批阻塞与拟定动作

为了让构建不受执行器断线的 SIGHUP 影响，准备在同一个正式 WSL 审批通道执行 `out/frame-policy-implementation-20261017T101525862/launch-background.sh`：有限 nohup 构建，2 jobs，写入项目 out 和现有 WSL 缓存，记录 PID、日志和退出状态，不安装依赖、不改账户/安全设置、不提交或推送。

自动审批拒绝了该动作，理由是它会后台启动构建/测试、写入并消耗资源，而最初任务要求只读审计、不得重型测试。拒绝与后续授权范围存在冲突；本轮遵守拒绝，没有换路径或重试。`rebuild-background.pid` 不存在，确认拒绝的动作没有启动。

需用户明确批准上述构建/验收及有限后台方式，再通过正式审批提交。获准后最小闭环：匹配库/应用重建、策略与配置单测、完整 `frame_policy_integration.py`、既有 probe/decoder/service/VVenC PTS/live_mux/media_hooks/process 回归、默认 skip 验证及匹配 Build ID 归档。完成后再交独立复审；当前不合并或发布。
