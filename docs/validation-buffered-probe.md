# 带缓存快速探流与 SDK 来源核对：第一批验证

独立审查随后发现并修复了旧 mtime 对象复用和预算边界问题；后续状态以 [审查修复记录](validation-buffered-probe-review.md) 为准，下方保留首轮结果及其来源。

基于父仓库 `e3343870fe9bfe219e6d69f20c19b93a90ed52da`、FFmpeg `f511375887a2226fd3971b6c2222785d264901e0` 的未提交实现；两个本地审查分支均为 `codex/buffered-live-probe`。环境为已有 WSL Ubuntu 24.04 x86_64 工具链，构建最多 2 个任务；没有安装依赖、推送、触发远端 CI、合入或部署。开始前的原实验备份在 `out/probe-before-implementation-20261016T204456869/`，原补丁和 `probe_test.c` 均保留。

## 实际实现

服务通过独立 `app/input_probe.c` 使用 fork 的显式 API 1，选择首条非附图 H.264/HEVC 视频，保留探测包；其他音轨和数据流可晚发现。不会在探流就绪后用 `av_find_best_stream` 换成另一条尚未就绪的视频。取消/读取错误保留错误语义；NOBUFFER/NOPARSE 拒绝。显式 live 路径不再 seek 到文件尾估计时长，普通 FFmpeg 客户端不启用此标志时保持原路径。libavformat 微版本为 63.3.103，服务拒绝更早的运行库。

SDK 来源清单记录依赖源码/配方内容摘要和实际安装头文件、库的 SHA256；构建期间输入变化拒绝封存。显式跳过构建时先验证来源及安装字节。发行缓存键要求已提交、匹配父 gitlink 的依赖源码及配方。测试用独立临时 Git 夹具验证这些规则；本地没有绕过 VVenC/yyjson Git 元数据权限。

## 本机重新运行的检查

构建及测试入口日志集中在 `out/probe-implementation-uAxSOt/`。最终 SDK 内容身份：`08cc5a4f1ce164541836ef03d63d61bcd1be9364633a3ae576dfdfaac1e7f174`。

| 检查 | 结果与新证据 |
| --- | --- |
| 匹配依赖与服务构建 | 通过，`build.log`；没有使用旧 SDK 编译新探流调用 |
| SDK 来源/复用规则 | 7 项通过，覆盖同 mtime 源码改动、新增源码、配方改动、安装产物改变/缺失、无来源清单、导入 SDK、脏依赖/gitlink 失配与未提交配方；`sdk_identity_test.log` |
| 快速探流专项 | 15 组媒体比较及 3 个错误模式通过，`out/probe-test-d2sb26b4/`。H.264/HEVC、8/10-bit、3B、分数帧率、单帧 EOF、MP4/TS 的完整视频包账本（PTS/DTS/大小/key/hash）及独立解码像素一致；第二条大尺寸/default 视频不会替换首条选择 |
| 晚轨发现 | 同上，TS 初始 1 条流→最终 4 条，Enhanced FLV 初始 1→最终 3；各 200 视频帧全部解出。初始化就绪不再被文件尾扫描伪装成完整拓扑 |
| 原解码回归 | 通过，`out/decoder-test-uj7u1tor/`；生成 B0/3、像素/元数据、生命周期、容量 1、多 owner 预算 |
| 基础服务/取消 | 通过，`out/service-test-swwz7f9y/`；文件、UDP、逐帧关联、停止、坏输入、输出排他、静默 UDP 探流取消和 SIGTERM |
| VVenC PTS GOP | 12 组通过，`out/vvenc-pts-pop807c9/`；VFR、空洞、非零起点、短 B 组与 EOF，独立 VVC 解码、PTS/DTS 和日志关联 |
| RTMP/SRT 直播封装 | 通过，`out/live-mux-8i77mc0c/`；VVC、两条晚 AAC、停顿恢复、同 worker/同连接、有界交织、MP3、关闭音频和纯视频 MP4 |
| 媒体 hook | 10 组通过，`out/media-hooks-gb2clbc_/`；串行 owner、引用留存、音频观察/copy 分离、编码重排/排空、失败与取消清理 |
| 显式复用与符号 | `NAKI_SKIP_FFMPEG=1` 复用验证通过（`reuse.log`）；应用 12、FFmpeg 6、VVenC 1 个 ELF 的匹配 Build ID DWARF 分别归档在本轮 `symbols/`，保留 `sdk-manifest.json` 与 `source-record.json`；源码仍未提交，清单明确标记 uncommitted |

现有 service CI 加入专项入口和证据导出，没有新增 workflow/job。Git 发行键夹具在 CI host 运行；没有 Git 的测试容器显式跳过该单项，其他 SDK 检查继续运行，不为测试安装 Git。

## 验证界限

本轮没有重跑 ASan/UBSan、冻结性能矩阵、Docker 镜像或远端 CI，没有生产容量、画质或长期运行结论。探流耗时只是诊断字段，不是源采集到显示时延，也没有绝对毫秒 SLA。内容和工具身份可核对，不代表不同工具链/软件源下位级可重现。

帧率为短前缀的名义估计，不能把它当成任意 VFR/脏输入的时间轴合同。源 PTS 严格透传、随源 I、最长 GOP 策略、过载抽帧、水印、多输出、动态视频规格、SEI 谱系和 SCTE-35 均仍待后续批次；响度算法仍不实现。下一批先处理时间轴/帧身份及策略模块，在提交层分开纯职责搬运与行为变化。
