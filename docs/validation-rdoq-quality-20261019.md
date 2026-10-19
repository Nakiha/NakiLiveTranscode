# RDOQ 画质首轮：可选恢复配置与实测边界

本轮在公开 CPU 账本分支 `e6280a2` 的同一源码/SDK 上，只增加两个可选 RDOQ profile。当前 `live_3b_2s_speed` 不变。树林短样本在接近实际码率下，全量 RDOQ=2 的 VMAF 增加 3.858、PSNR 增加 0.099 dB；选择性 RDOQ=2 增加 3.929、0.103 dB。合成运动/边缘样本在补齐实际码率后，VMAF 增加 0.234，PSNR 减少 0.033 dB。保留这两个候选供画质阶段继续验证，不据此宣称所有内容、每帧或直播容量都改善。

## 配置与既有证据

| 工具 | speed 基线 | 本轮处理 / 已有证据 |
| --- | --- | --- |
| RDOQ / SelectiveRDOQ | 0 / 0 | 新 profile `live_3b_2s_rdoq2` 为 2/0，`live_3b_2s_rdoq2_sel2` 为 2/2；模式2来自 faster preset。没有测试 RDOQ 模式1。 |
| RDOQTS | 1 | 基线已开，本轮没有恢复或独立开关消融。 |
| ALF / CCALF | 0 / 0 | 本轮未改。历史 H.264 输入的 `rdoq0` 配对回收约 1.54849 VMAF，但约27.978fps、最大442.146ms；两滤波工具未分别归因。 |
| BIM | 0 | 本轮未改；保留 P-anchor/MCTF、320×180 10-bit 的已知不兼容回归，未声称修复。 |
| QPA | 1 | 基线已开，未见独立关闭/恢复画质对照。 |
| MCTF | 1，future=0 | 只用过去参考；有 no_mctf 实现选项，本轮没有单独质量消融。 |
| SAO | 1 | 基线已开，本轮没有单独质量消融。 |

历史 qt16 的 RDOQ/ALF 保留组合与 speed 的 VMAF 74.020457/69.741624 是多工具、H.264 输入对照，不能作为本轮 HEVC 或 RDOQ 单工具的数值。历史记录见 [受限CRF报告](validation-constrained-crf-20261018.md)。本轮 `effective-{base,rdoq2,sel2}.json` 用同一库 replay 应用/wrapper 参数并执行 encoder_open/get_config，最终字段只在 RDOQ/SelectiveRDOQ 上不同；不是对历史运行补造的配置 dump。

## 固定边界

1080p60、GOP120/3B、8-bit HEVC 输入，服务选择540p30、10-bit VVC，源I跟随、GOP4/3B、future guard4（各轮实测最大3），faster、8编码线程、QT16/chroma8、SearchRange64、MinSearchWindow32、FastIntraTools2。QPA/SAO/过去MCTF保持开启，ALF/CCALF/BIM关闭；native RC LookAhead=0、MaxParallelFrames=0。`encoder_input_bit_depth=10`、`encoder_bit_depth=10`、`encoder_native8_reference=false`。受限CRF、源/绝对双预算、maxrate1100000、解码预算2、原有有界队列、音频AAC透传和时间戳不变。`BUSY_WAIT_TIME=0`、CPU账本关闭。

树林使用既有两遍ABR目标8Mbps源的720帧/12秒scout；短切片的实际输入码率随ABR内容分配变化。合成样本为12秒 `testsrc2` 运动图形/边缘，单独两遍ABR目标8Mbps，实测视频载荷7.887710Mbps。它不是第二段自然内容，不能代表一般场景。

| 输入 | 原始FLV SHA256 | runner准备后SHA256 |
| --- | --- | --- |
| forest scout | `33d0038c5cea0513652dff6b9df72bd9ddd6302433e94218144804fc605f4d0f` | `5e4c7ecd6fd222fcf38bef37be6e6a11cac499d0374258c1b8b639ef8527b8d3` |
| patterns | `ed8ea8db200e80899c931ea50a73ac54d53e0aa8f929485154e7afa9f29d1ae7` | `3bd2413d34759864c6ec08f205363f7def08634fabc8c3c0f4af6ec52bdc0682` |

SDK源内容身份 `70b2aab29a8005de62d7c9b1863e2ed7b91e4b573112aace8c225bea3fa44559`，VVenC公开pin `42cbd92067e937fb90cbda6bf73e11409ee539a8`，worker SHA256 `92cdfff5e4d344eac0d162594bebd28d6039a1f3917b65864b95befabcf3470a`。服务构建复用SDK，没有重编或发布SDK。独立期望清单绑定公开gitlink与构建配方，dependency key为 `7760fdb85fe2e03be3baf5c7`，其验证通过。

## 全帧质量与码率匹配

每轮实际输出337帧，全部按精确PTS评分；10-bit、峰值1023的pooled-MSE PSNR，`vmaf_v0.6.1`、原生960×540、n_subsample=1。参考为解码后的HEVC输入经相同bilinear过滤器缩放；不包括输入制备损失、60→30流畅度、音频和显示。各轮16帧身份检查及完整帧数/序号检查通过。

视频平均码率按AVPacket载荷加一次codec configuration除以名义输出时长；不含音频/封装。先同CRF32筛选，再针对合成内容补一个基线CRF31实测点；没有插值。树林候选码率相对基线增加0.65%/0.61%，合成匹配候选减少0.46%，都在预先声明的3%近似匹配范围内。

| 内容 / profile | CRF | 视频kbps | PSNR dB | VMAF mean | VMAF P05 |
| --- | ---: | ---: | ---: | ---: | ---: |
| forest / speed | 32 | 847.087 | 31.226788 | 74.073033 | 60.233423 |
| forest / RDOQ2 full | 32 | 852.600 | 31.325940 | 77.930968 | 63.360439 |
| forest / RDOQ2 selective2 | 32 | 852.215 | 31.329709 | 78.002157 | 63.922901 |
| patterns / speed，初筛 | 32 | 393.416 | 45.218753 | 95.841046 | 94.370320 |
| patterns / speed，匹配点 | 31 | 429.802 | 45.482621 | 96.117625 | 94.785778 |
| patterns / RDOQ2 full | 32 | 427.817 | 45.449373 | 96.351749 | 94.966300 |
| patterns / RDOQ2 selective2 | 32 | 427.817 | 45.449373 | 96.351749 | 94.966300 |

树林337个共同PTS上，全量RDOQ的I/P/B均值VMAF差分别为+7.651/+4.583/+3.519，不只I帧有收益。但15帧分数下降、10帧下降超过1分，最差逐帧差−5.323；选择性候选13帧下降，最差−3.991。两候选均值差0.071不足以作普遍优劣判断，尚未重复验证。

具体画面自动选择最大增益、最大退化、基线最低分和中位增益四帧，保存全图及中央480×270的2倍最近邻展示。frame300/PTS11处，全量RDOQ保留了更多地面叶片纹理；frame308/PTS11.267处存在局部模糊/纹理变化，不能将均值提升解释成逐帧改善。该静帧检查不是盲测或播放流畅度验收。图与选择规则在 `out/quality-rdoq-20261019/visual-forest-v2/`。

## 实测成本

CPU是service进程树采样累计CPU除以输出帧数，不是编码器单独CPU。时延为服务首次相关网络读取到实际mux/write/flush，含排队；不包括首个相关读之前的时间，也不测显示时间，不能称为线上传输或source-to-screen时延。

| 内容 / profile | CPU ms/帧 | 时延mean / P99 / max ms |
| --- | ---: | ---: |
| forest / speed32 | 106.825 | 171.546 / 243.115 / 243.811 |
| forest / RDOQ2 full32 | 108.605 | 173.235 / 245.248 / 254.582 |
| forest / RDOQ2 selective2-32 | 108.813 | 172.920 / 245.610 / 251.179 |
| patterns / speed31 | 78.843 | 156.579 / 222.646 / 232.790 |
| patterns / RDOQ2 full32 | 80.801 | 159.318 / 227.152 / 241.576 |
| patterns / RDOQ2 selective2-32 | 80.653 | 159.337 / 226.698 / 238.857 |

全量RDOQ相对匹配基线的CPU观察值增加约1.67%/2.48%，平均时延增加约1.69/2.74ms。均为单次短测，不能给出噪声之外的普遍成本结论。含启动/停止的实际输出约29.42–29.47fps；paced输入和极窄去头尾窗口不测饱和吞吐，也不能证明多路容量或长时实时性。七轮码率/未来画面/源I/3B及音频审计通过，无源/绝对预算违规，AAC丢弃0。没有以性能门槛淘汰画质候选。

## 复现、交付和下一轮边界

所有媒体、逐帧指标、sealed trace与失败预检均保留在忽略的out目录，不入Git。一次结果路径预检在媒体启动前失败，日志另存 `preflight-result-path.log`，没有覆盖失败现场。服务构建及control-spec/frame-policy-config两个测试通过；七个runner与七个质量COMPLETE均通过。

- 编码入口：现有 `scripts/bench-service.py rtmp`，`--repeats 1 --seconds 120 --mode paced --input-fps 60 --input-gop 120 --audit-burst-bits 1100000`，配置与逐组命令见 `out/quality-rdoq-20261019/run-screen.sh`、`config-*.json`。
- 质量入口：现有 `scripts/check-video-quality.py`，4个metric线程，所有帧评分；命令见同目录 `run-quality.sh`。
- 初筛汇总：`out/quality-rdoq-20261019/screening-results.json`；合成内容实际码率匹配：`rate-matched-patterns.json`；逐帧类别差异：`frame-deltas.json`。
- 原始runner证据：本实验工作树的 `out/quality-rdoq-20261019-{forest,patterns}-{base,rdoq2,sel2}-crf32/summary.json`，以及 `patterns-base-crf31`；对应质量目录为 `out/quality-rdoq-metrics-20261019-*`。

本轮成本限于7次12秒编码测量、7次全输出帧质量计算和两次复用SDK的服务构建调用（首次构建、随后增量修正配置名并加测试），未扩展测量框架或进行调度/性能重构。保留两个可选RDOQ候选及配置，默认直播speed不切换。下一画质轮先在更长自然内容和更多真实场景确认RDOQ收益/最差帧，再以固定RDOQ候选为基准单独恢复ALF/CCALF；BIM先验证其已知不兼容边界。性能阶段尚未开始，须先冻结质量目标。不得通过增加未来帧、改分辨率/帧率、preset、线程或码控掩盖工具影响；若PTS、帧数、预算或future审计失败，先修复该轮而不组合新工具。
