# 后续I帧预算与GOP派发细分（2026-10-18）

本批沿用上一轮的HEVC ABR 8Mbps、1080p60、GOP120、3B树林输入与540p30输出，固定crf32、1100kbps、源I跟随、3B、未来画面守卫4、speed工具配置、8编码线程和现有队列。没有改变preset、RDO搜索、ALF/RDOQ、MCTF或解码器集成。所有新媒体、日志和结果只写独立out目录。

最终D将后续29张I的平均VMAF从55.851463提高到66.042536，3秒最差I从25.642294提高到59.759118；整段PSNR30.869758dB、VMAF71.095557。三次正式有限回放全部满足1100kbps与300ms审计，最大时延257.236/262.438/250.159ms。仍有43秒I低谷35.822301，P平均VMAF有小幅让步，详见下文；不是所有帧都变好，也不是无条件硬保证。

## 最终D性能与验收

同一60秒源，有限回放达到1770张目标后API停止，各实际写出1781张（PTS1.000–60.333秒）；尾部全部按自己的精确PTS验收/计分，没有把API停止当正常EOF。完整EOF由240→120张功能用例另验。时延边界是app首次相关RTMP网络读取到实际write/flush；不是线缆时间或播放器显示。环境为Ryzen7 9700X、WSL2/Linux，单路paced loopback，BUSY_WAIT_TIME=0；中段fps排除首尾各5秒，不能据此作多路容量承诺。

| 轮次 | codec_trace | 中段fps | 平均/P95/最大时延ms | >300ms |
| --- | --- | ---: | ---: | ---: |
| off1 | false | 29.9594 | 172.056 / 236.549 / 257.236 | 0 |
| on | true | 29.9586 | 172.899 / 238.080 / 262.438 | 0 |
| off2 | false | 29.9853 | 172.534 / 237.136 / 250.159 | 0 |

| 轮次 | 视频均值kbps | 墙钟滚动1秒峰值bit | DTS滚动1秒峰值bit | 双桶超支/最大债务bit | AAC包/丢弃 |
| --- | ---: | ---: | ---: | ---: | ---: |
| off1 | 828.882 | 1035824 | 1028912 | 0 / 0 | 2788 / 0 |
| on | 828.591 | 1038872 | 1031504 | 0 / 0 | 2787 / 0 |
| off2 | 829.167 | 1035824 | 1028912 | 0 / 0 | 2783 / 0 |

视频口径为AVPacket payload与一次codec config，排除AAC和FLV/RTMP开销；沿用声明的1.1Mbit桶容量/启动授信，另独立检查严格滚动1秒窗口。三轮都保留30张源I、最多连续3B、仅首IDR；实际未来画面最大3≤4。解码3567张、主动60→30丢弃1783张、过载丢弃0。此8Mbps源没有令源码率预算成为主要瓶颈，低源码率/突降不能据此无条件外推。

同一新二进制off/on/off中，on比两轮off均值多0.604ms总时延、约1.06%采样服务CPU；峰值RSS约188.07/188.73/188.50MiB。观测开销有限但非零，正式性能以off为准。墙钟反馈使实际受限运行码量/画质随时序轻微变化；高cap功能用例的逐包payload/PTS/DTS开关等价检查另行通过。

## 画质低谷的依据与修改

上一轮关闭诊断基线`out/rtmp-bench-t4gtch91`：源PTS3秒的后续I请求QP46、实际slice QP45，预测90613bit，实际2637字节（21096bit），预测高约4.30倍，VMAF25.642294。5/7/9秒的后续I也被压到高QP。原模型将首个IDR与后续普通TRAIL I共用一个系数，初始0.8，每次I仅20%更新，误差最多向下修正到旧值的1/4；2秒一张I，过估会持续多个GOP。模型以请求QP建模，实际slice QP仍由QPA/层级调整，不把两者混同。

改为分别维护启动IDR与后续普通I的模型：首IDR保留原0.8先验，B/C普通I以0.20启动，D改为0.40以减小首个普通I的低估风险，I反馈50%更新，其余类型20%。这些是实验模型先验，不是每个素材的真实固定系数。后续I不再继承启动IDR反馈；仍逐张用真实AU码量修正。

B/C让I使用扣除预测在途预留后窗口余量的80%，P/B沿用40%；D进一步为随后P+3B保留名义目标额度（最多取当前余量60%），I使用剩余部分；首个预测P参考锚帧可用其剩余窗口的85%，后续P/B仍40%。I目标权重仍6，最大目标仍82%预算×帧周期×权重，窗口保守边界仍95%。这是联合预算预测，不新增token信用、不预读未来画面，也不增加前视、等待写出、重编码或额外丢帧。

短组末尾的预测B可能实际成为P，现在码量反馈按真实输出I/P/B更新对应模型，预留释放仍绑定原输入，避免短组P污染B模型。trace增加`rc_model_kind`（普通I0/P1/B2/启动IDR3）、`rc_model_coefficient`、`rc_window_room_bits`，方便复算预测和预算来源。

新增native检查覆盖首IDR与后续I反馈隔离、普通I保真、独立预算响应、实际短组P反馈、在途预留替换与释放及非法类型拒绝。历史冷启动I逐帧证据在`out/i-budget-20261018-a/baseline-i-diagnosis.json`，原模型头文件单独保存；不改历史结果。

## 第一候选完整轮暴露的边界

B小样通过，但完整off1 `out/rtmp-bench-b3o1zmcq`未通过：一张超过300ms，最大307.474ms；一个DTS滚动1秒窗口1111608bit，墙钟峰值1099192bit，双桶没有超支。自动顺序执行器停止后续on/off/质量测试，这个失败窗口原样保留。

DTS问题有直接证据：源PTS2.067秒的P被预测到约2.000秒的码控时钟，提前从历史中排除了DTS0.933秒的启动I；该P实际写出DTS是1.900秒，启动I仍在其滚动1秒窗口里。导致允许预算从约22.7kbit跳回96.2kbit，实际P由小样的1381字节变成17563字节。源I改进不是这个启动DTS误计问题的根因。

C候选用已编码完成AU的真实DTS推进媒体预算时钟，按实际输出time base舍入，输入PTS不再自行释放信用。已编码未写出的AU继续留在pending预留，写出后才转入实际历史；尚未完成接收的源字节不参与。新增`rc_media_clock_us`，独立auditor逐张验证它等于该决定之前最近已完成编码包的DTS。native额外检查远在前面的输入PTS不能移除仍有效历史、重复/回退编码DTS必须失败。

307ms帧为frame_id773、源PTS13.867秒、输出B：decode驻留143.959ms、encode119.546ms、mux41.540ms。该轮关闭内部打点，不能把decode驻留当作CPU或断言全由I预算导致。C将配置的mux_wait_ms从50降到20以减少可控等待；队列容量、I/3B、未来守卫及编码工具不变，音频迟到丢弃必须另验，不能以音频丢失换时延达标。

### C画质对照后的进一步修正

C完整off/on/off均通过合同：最大258.751/259.224/252.293ms、墙钟峰值1079200/1072544/1079200bit、DTS峰值1008952/1003008/1015416bit。AAC各2787/2788/2787包，无音频丢弃；codec_trace完整关联1781输出、3567解码画面。

但不能只看I：C off1的I平均VMAF从56.495307升至76.834027，最差I66.414430；整段VMAF70.868230，较基线70.928654基本持平。13.133秒的随后P被压到实际QP46、VMAF35.134304；诊断on轮同一P实际QP52、VMAF19.570614。说明I吃掉过多余量后，把局部低谷移到了参考P。D增加后续参考组预算保护与首参考P优先，并提高普通I冷启动先验；`rc_followup_reserve_bits`记录留额。留额不是额外授信，预测误差仍可侵占它，最终以真实窗口验收。

C所有结果保留，`out/i-budget-20261018-c/code-snapshot.zip`中的212份代码逐文件匹配C gate的原始hash（`10cf6944f827716aae0db7d87c40177b0c9988087ba0b2a857edf993831223b0`），不拿D代码重新标记C证据。

## GOP派发的可核验边界

VVenC诊断ABI升为2，decoder ABI仍为1。每张图新增：GOP admission、slice初始化起止及调用线程CPU、前一张串行编码画面的真实重建发布时间；后者由原finalize回调以原子时钟发布，不用于调度。库/FFmpeg/app必须匹配重编。

GOP admission→dispatch拆为下列不重叠区间，和必须严格等于原间隔：

| 区间 | 解释 |
| --- | --- |
| MCTF完成→admission | 阶段传递/图像初始化入口之前 |
| pre-init predecessor | admission到slice初始化前，与串行前图尚未完成重叠的部分 |
| pre-init available | 串行前图已完成、尚未开始slice初始化的剩余部分 |
| slice init | xForceScc/首slice与参考列表初始化的墙钟，另记CPU |
| post-init predecessor | 初始化之后仍与前图未完成重叠的部分 |
| post-init available | 初始化后到dispatch的其余部分 |

auditor按真实coded order逐张验证predecessor时间等于上一张的reconstructed事件；不能按显示PTS猜测。predecessor重叠是串行约束驻留，不能一律称为锁等待；available仍包含上层再次推进、未单计的条件检查及少量CPU，不冒称纯空闲。slice初始化CPU未覆盖此前QP cascade的CPU，剩余区间会保留这部分。

新增整数计数为每张图GOP访问次数，以及实际被拒绝的coding-order、pre-analysis、rate-cap、参考重建、APS、空闲picture encoder与RC条件。它们不是持续时间或互斥归因；位于队首后、未被检查的图不虚构拒绝次数。原short-circuit检查只执行一次，不增加引用探测，不在ready probe读时钟，不改变编码决策。CTU及logger所有权沿用上一批。

## 验证方法

固定原HEVC网络输入SHA256 `decaae53011dcc8766b21123144b85aec5075eb6d2b256001f2200bdaaf59968`，不重新制备或提高源质量。先运行功能和默认路径回归，再诊断scout，正式off/on/off串行完整回放。每轮保留码率、逐帧时延、帧型/PTS、未来帧数、音频及source/binary/SDK身份审计。任何候选合同失败保留其窗口并停止后续重负载测试。

PSNR/VMAF使用每张出流的精确PTS对齐同款bilinear 540p10-bit参考，全部实际输出画面计分，包含取消尾部；原生540p、VMAF v0.6.1、n_subsample=1。旧基线对照是同一HEVC源的冻结结果，不冒称同一二进制重编的严格随机A/B。诊断开关仍用同一新二进制做功能payload等价检查，并以off/on/off量化观测开销。

## 最终D诊断：GOP驻留的去向

完整on轮`out/rtmp-bench-6fgmlsrd/1/codec-phase-audit.json`，1781张写出逐帧核验。下表按实际输出类型，均为每帧平均ms；I包含首I，后续I另列外层时延。前图身份按coded order验证，所有分段和都严格闭合。

| 阶段 | I | P | B |
| --- | ---: | ---: | ---: |
| MCTF完成→GOP dispatch总驻留 | 49.310 | 0.402 | 50.004 |
| 其中阶段handoff | 0.008 | 0.389 | 6.642 |
| 其中与串行前图未完成重叠 | 49.093 | 0.000 | 42.930 |
| 前图完成后→slice初始化 | 0.196 | 0.006 | 0.419 |
| slice初始化墙钟 | 0.012 | 0.006 | 0.011 |
| 初始化后剩余驻留 | 0.001 | 0.001 | 0.001 |
| 组内未来输入等待（另一个阶段） | 0.000 | 0.000 | 42.986 |
| 实际CTU首末任务跨度（另一个阶段） | 16.319 | 24.533 | 11.468 |

I的49ms主要与已经排在前面的串行图计算/完成链重叠。它没有等待未来I分析输入；在当前非FPP路径，队首图完成后才逐张初始化后图，后图驻留发生在这些ready检查之前。本轮共4451次GOP访问，单图最多4次；coding-order、pre-analysis、rate-cap、参考、APS、空闲encoder、RC拒绝计数均为0。零拒绝不能解释成不存在依赖，而是没有在对应检查点发生重试。

B的阶段handoff还包含同一批次后续锚图MCTF工作完成前的传递延迟，不应把6.642ms全称为分配或锁开销。前图已完成后的剩余驻留均值很小；把整个49–50ms归因于PARC或poll没有实测依据。若下一步继续压这段，重点应是安全的串行图流水重叠，保持参考就绪与future guard，不能绕过同步来换延迟。

| 外层分类 | 张数 | encode平均/最大ms | 首相关网络读取→实际flush平均/最大ms |
| --- | ---: | ---: | ---: |
| 首I | 1 | 31.142 / 31.142 | 105.938 / 105.938 |
| 后续I | 29 | 86.068 / 96.276 | 124.625 / 136.324 |
| 输出P | 445 | 38.103 / 65.007 | 109.900 / 142.208 |
| 输出B | 1306 | 109.399 / 145.923 | 195.488 / 262.438 |

编码器之外逐帧剩余时延平均81.737ms、最大166.464ms；decoder完整驻留包含显示重排，NAL窗口均值7.134ms、调用线程CPU6.296ms、引用等待路径0.861ms、显示重排51.485ms。mux驻留均值9.479ms、最大35.267ms；配置20ms是交织队列的到期预算，输出owner运行其他工作和OS调度仍会使实际写出驻留超过20ms，所以合同始终按真实flush验收。

## 最终D画质与取舍

`out/quality-i-budget-off1-20261018-d`及`out/quality-i-budget-on-20261018-d`均完成1781张的全量检查；与旧HEVC off1的所有精确PTS集合相同。逐帧配对结果在`out/i-budget-20261018-d/quality-comparison.json`，可由同目录`compare-quality.py`复算。参考是解码后的HEVC源经同款540p10-bit缩放，不包含相机到8Mbps制备损失或60→30时间采样损失。

| 轮次 | PSNR YUV平均dB | VMAF均值 | VMAF P05 | 最低单帧VMAF |
| --- | ---: | ---: | ---: | ---: |
| 冻结旧基线off1 | 30.835189 | 70.928654 | 56.831697 | 25.642294 |
| D off1 | 30.869758 | 71.095557 | 57.605831 | 35.822301 |
| D on | 30.875312 | 71.154954 | 57.886966 | 44.631340 |

正式off1相对旧基线为PSNR +0.034569dB、VMAF +0.166902。主要收益是分配与低谷改善，不能称为平均画质大幅提升；也不能把on比分数稍高解释为打点在优化编码。

| 精确PTS配对分类 | 张数 | 旧平均VMAF | D off1平均VMAF | 配对差值 | 旧最低→D最低 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 后续I | 29 | 55.851463 | 66.042536 | +10.191073 | 25.642294 → 35.822301 |
| 首I | 1 | 75.166774 | 75.166774 | 0 | 75.166774 → 75.166774 |
| P | 445 | 77.319254 | 76.792468 | −0.526786 | 50.580615 → 47.668701 |
| B | 1306 | 69.082700 | 69.263505 | +0.180805 | 40.249112 → 45.410871 |

3秒I实际QP45→37、2637→11360字节，VMAF25.642294→59.759118。13秒I后的13.133秒P从C候选实际QP46（on达52）降至D off1 QP33，其码量139264bit；不再为I保真而立即把这张参考P压至高QP。

仍有需要继续处理的情况：D off1的43秒I可用窗口135327bit，给后续组留81196.2bit后，I allowance54130.8bit、请求QP44/实际QP43，VMAF35.822301；51秒I实际QP40、VMAF47.271991。时间节奏变化下D on的最差I47.669897，说明滚动窗口边缘和预测误差仍会令分配波动。下一步应改善真实GOP角色/短组预测、码量置信度及连续预算平滑，而非单纯提高I固定权重。每项还须同时守住真实窗口、前视和最大时延。

成功后原始参考/失真YUV已删除，仅保留压缩出流、对齐索引、逐帧PSNR/VMAF、命令和哈希；identity 16张PSNR为inf、VMAF均值99.839268。

## 代码、依赖与复现证据

父仓库依赖HEAD `a3e7fcca3d0f0c1cae6c2f9e6e243611553a421f`；VVenC `cfc91dada26fbef4957d2a114260a048cbee2ff5`、FFmpeg `ed8b95e1088a4ac2ae3974a34a58bca0eb95a142`均在各自fork提交并推送，再更新gitlink。初次库编译的新增计数分支被Werror拒绝，修正缩进后从新B构建窗口完成，没有复用失败构建标记。

- dependency key：`1d78e3e6bf17019f9ea14509`。
- SDK identity：`d0a6670d7122a355c064c593962aa61366eb1590e083983eefb041901f86cfd1`。
- 最终D应用/脚本/测试snapshot：`62e11ef6d3edad14c5105467e2a04667f5694901254b2f6b34155d6b4090d3e3`。
- VVenC Build ID：`d1021b947ae5da1d1f9f0f5e47dde51ac17de962`，匹配DWARF及库provenance在`out/i-budget-20261018-b/symbols-vvenc`、`symbols-ffmpeg`与`dependency-provenance.json`；后续C/D只改app，不重建这些库。

正式构建经`scripts/build-service.sh`；C/D在验证SDK未变后用其`NAKI_SKIP_FFMPEG=1`路径只重建服务。最终D功能窗口`out/checks-i-budget-functional-20261018-d`的8项通过：SDK、native16/16、organization、codec-phases、live-budget、entrypoints、cqf-rate、read-origin。未再改变的库及default路径回归保留B的5项通过窗口`out/checks-i-budget-regression-20261018-b`：SDK、lifecycle-media、vvenc、vvenc-pts、live-mux；不将其不同app hash冒充D重跑。正式每轮source/binary/SDK前后均未变，性能与离线质量不并跑。

| 最终轮 | benchmark目录 | 独立输出与manifest |
| --- | --- | --- |
| scout | `out/rtmp-bench-r_nb8jgs` | `out/network-output-i-budget-scout-20261018-d` |
| off1 | `out/rtmp-bench-i2mxwddd` | `out/network-output-i-budget-off1-20261018-d` |
| on | `out/rtmp-bench-6fgmlsrd` | `out/network-output-i-budget-on-20261018-d` |
| off2 | `out/rtmp-bench-cpqcob3d` | `out/network-output-i-budget-off2-20261018-d` |

配置/顺序执行脚本在`out/i-budget-20261018-d/full-trace0.json`、`full-trace1.json`、`run-d.py`，汇总`summary-d.json`；使用示例为`examples/rtmp-vvc-constrained-crf-540p30.json`及`rtmp-vvc-codec-trace-540p30.json`，显式mux_wait20。源代码/文档封存于`out/i-budget-source-20261018-final`，含binary-safe父patch、逐文件/ZIP校验及测量manifest哈希。输入、出流、日志、库与符号全部留在Git忽略目录；未修改用户文档`docs/next-metadata-batch-and-release.md`。
