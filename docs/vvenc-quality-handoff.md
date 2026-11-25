# VVenC 画质验收交接

2026-11-25 核对。接手时用 R28 `9b7bddb2` 作画质对照，Live 和 Quality 各与自己的 R28 档位比较。现有画面门禁覆盖 11 份源画面，每档 13 个「片源 × 帧并行度」组合、2,856 帧次。腾讯原片另用于主观对照和实测语法子集检查。

[可机读片单](../experiments/vvc-tool-screen/r34-quality-gate-sources.json)记录 11 份源的完整 SHA-256，以及两个档位各 13 对**实际选中**的 R28 / 候选码流、字节数、SHA-256、码率差和评分回执。路径分别相对清单中的 `source_root` 和 `evidence_root`。搬机器时先用它确认拿到的是同一批输入和对照。

## 当前验收到哪里

| 档位与代码提交 | 同码率客观画质 | 最差组合的平均指标差 | 新增严重色度格 / 跨帧持续格 | 全片腾讯子集及双解码 |
| --- | --- | ---: | --- | --- |
| Quality 交付候选 `patch-stage-duel`，`39603f9a` | 13/13 | −0.1816 dB | 0 / 0 | 13/13，2,856 帧次 |
| Quality 加速候选 `deblock-integer`，`7a16508d` | 13/13 已测 | −0.1925 dB | CH09 有 1 个孤立格 / 0 | 13/13，2,856 帧次 |
| Live 加速候选 `joint-residual`，`e448ab09` | 13/13 | −0.1813 dB | 主轮 0 / 0。CH09 三次独立闭环重跑分别为 1、1、2 个孤立格，持续格均为 0 | **9/13，2,307 帧次** |

Live 待补 `ch09`、`ch14`、`finger`、`game12-100` 的 P4，共 549 帧次。报送时核对所测 SDK、实际码流 SHA 和覆盖项，尤其要带上 CH09 的重复试验。当前清单的 Quality 列绑定交付候选，另一版 Quality 的选流见它自己的 `panel-quality.json`。

## 门禁片源和回看位置

全部是 **540×960 竖屏、30 fps、8-bit planar 4:2:0（I420）**，每帧 777,600 字节。下表的 P4 / P8 指最多并行编码 4 / 8 张画面，当前编码配置使用 8 个线程。帧号从 0 起，按窗口内的显示顺序数。坐标从左上角起算，单位为亮度像素。

| 片源 | 帧数 | 并行度 | 用途及定位 |
| --- | ---: | --- | --- |
| `finger` | 60 | P4 | 内网手指回归。逐帧回看 #12–15，Quality 重点 #13，Live 重点 #14。看整只手的轮廓、指间和小黑标记。 |
| `ch01` | 87 | P4 | 内网频道 01。与腾讯对齐的源显示帧 5816–5988，每两帧取一帧。 |
| `ch09` | 135 | P4 | 频道 09，源帧 6–274，步长 2。回看 #39 字样 (440,16)、#117 手背 (352,280)、#118–119 手腕 (304,352) 的 8×8 区域。出现退色或发绿时重跑闭环。 |
| `ch14` | 54 | P4 | 频道 14，源帧 3373–3479，步长 2。覆盖短尾，片长不足一个 60 帧 IDR 周期，也不足 5 秒码率窗口。 |
| `11`、`13` | 各 300 | 各 P4、P8 | 每份都测两档帧并行度。另有带时间戳的服务链输入，用于检查台架结果能否在实际服务输出复现。 |
| `05C`、`10` | 各 300 | P4 | 跨片验证。05C 可回看 #251 附近 (288,416) 的局部色偏。 |
| `S4-100` | 120 | P4 | 两个完整 60 帧周期。片单中的 `S4-100` 和下文冻结的腾讯结构 trace 分开保存。 |
| `game12-100`、`900s-100` | 各 300 | P4 | 取长输入的 300 帧窗口。游戏片回看 #202 附近 (272,464) 和 #206 的逐帧低谷。 |

手指小样的压缩原片在 Windows 的 `D:\video\q26-test-streams`，`source_gop540_599.h265` 和 `tencent_gop540_599.h266` 成对，旁边有 R26L / R26Q 供追溯。验收源是按 `scale=540:960:flags=area` 处理的 `source-area.i420`。可用三个框分别看标记 (144,216,8,8)、反馈位置 (128,192,32,32)、整只手 (84,180,172,194)，并播放连续帧。

频道 01 / 09 / 14 的成对原流在 `D:\video\q26-pull`，文件形如 `09_in.flv`、`09_out.flv`。处理好的 `source.yuv` 旁有 `window.json` 和已对齐的 `tencent.yuv`，记录了原始帧号、抽帧相位和 bicubic 缩放。要扩充片单，可从 `vvenc-q26-pull-forensics-20261123-01/inventory.json` 和 `windows_q26.py` 追到原始输入。该目录另存的 `recommended_benchset.json` 是历史 05、10、11、13 四频道基准。

## 怎样判断画质

| 检查 | 做法与判读 |
| --- | --- |
| 输入、码控与帧对齐 | 同一源 SHA、档位、帧数、并行度和码率上限，走正常闭环码控。按两个 VVC 裸流的实际字节数配对，要求 `abs(candidate_bytes / r28_bytes - 1) ≤ 0.005`。30 fps 时平均 kbit/s 为 `bytes × 8 × 30 / (1000 × frames)`。未配上记为未测，保留每次尝试及最终选流。固定 QP 重放用于定位。与腾讯比较也要列出实测码率。 |
| 平均画质硬门槛 | 每个组合取四个候选减 R28 的差值。分别是整段 Y-PSNR、整段 YCbCr 合并 PSNR、逐帧 Y-PSNR 差的均值、逐帧合并 PSNR 差的均值。合并前在 MSE 域按 Y:Cb:Cr = 4:1:1 加权。四者的最小值 `min_guard ≤ −0.20 dB` 即拦截。另留各分量、逐帧最差值和第 5 百分位，方便找平均值掩盖的坏帧。 |
| 局部杂色硬门槛 | `color_regions` 在源 Cb、Cr 各取互不重叠的 4×4 格，对应亮度 8×8。源格范围 ≤8、候选的有符号均值误差绝对值 ≥12、R28 同格 <8，记为新增严重格。上一帧同位置或相邻格有同符号新增时记为持续，`repeated > 0` 拦截。单帧孤立格记录帧号、平面和坐标，回看原图及前后帧。人工确认明显新增杂色同样拦截。 |
| 泛绿、灰化与边缘 | `cast-scan` 在色度平面上用 4×4 滑窗、步长 2，以 `−1.2694 × ΔCb − 1.4292 × ΔCr` 找向绿偏移，报告 7 / 14 两档、跨线增减及连续命中。用它定位，再看原画、R28、候选、可用的腾讯同帧，统一缩放和显示转换，核对肤色、轮廓和运动中的闪动。此项没有已校准的通用数值放行线。 |
| 腾讯同角色语法及双解码 | 在**实际送测码流**上全片检查。腾讯 S1、S4 各两个完整周期，加 mini8 共 248 帧，按相对最近真实 IDR 的角色建立参考。覆盖 CU 划分、预测及有序 L0/L1、AMVR、merge/MMVD、SBT、各平面 TU / transform skip、CCLM、亮度及色度 SAO。扩展检查器再核 SPS/PPS/PH/SH、WPP、RPL 和预测。VTM 与独立 FFmpeg 对可见原生位深像素的 SHA-256 须相同。4×8 / 8×4 要按实测角色判断。 |
| 服务与任意 IDR | 真正送帧出口另验随源强制 IDR、最大间隔和相邻 IDR。91 帧用例的随源 IDR 为 0、3、12、80、81、90，带最大间隔时还出现 #56。任意 IDR 用按实际调度切段的回执。常规 60 帧网格的通过记录只覆盖常规网格。 |

局部色块扫描只统计能容纳完整色度格的平坦区域。纹理、指缘和低于阈值的整体退色都需要连续回看。当前 13 组没有统一 VMAF 放行线。需要补测时，`vvenc-r25-20261124-01/run.py` 的 `score` 示范了同源打分，已有的 `/var/tmp/vvenc-r19-20261123-01/tools` 包含 `vmaf`、`vmaf_v0.6.1.json` 和 `vmaf_v0.6.1neg.json`。手指并排图的实际码率为 R28 Quality 1268.0、R34 Quality 1265.5、腾讯 1177.7 kbit/s。前两者可作同码率比较，腾讯列可看手的轮廓与肤色。[历史 PSNR / VMAF 流程](validation-quality-cqf-20261018.md)

## 工具和证据放在哪里

以下路径按本机 WSL 布局书写。`OUT=/mnt/d/Code/ffmpeg_live/out`，对应 Windows 的 `D:\Code\ffmpeg_live\out`。

| 位置 | 接手时使用的内容 |
| --- | --- |
| 仓库内 [vvc-tool-screen](../experiments/vvc-tool-screen/r28-geometry.md) | 可版本追溯的 `check-r28-geometry.py`、`test-r28-geometry.py`、AWK 过滤器和两份冻结参考表。`$OUT/vvenc-r17-tencent-vtm-20261122-01` 保存了可重新计算参考表的原始 trace。 |
| `$OUT/vvenc-r29-kernel-trials-20261125-01` | `picture.py` 用源、R28 和候选逐帧打分。`pair_scores.cpp` 输出逐帧三分量 PSNR、汇总及局部误差。 |
| `$OUT/vvenc-r30-search-budget-20261125-01` | `closed.py` 计算合并 PSNR，`color_regions.cpp` 扫描局部严重色差，`check_event_color_regions.py` 汇总新增与持续格。 |
| `$OUT/vvenc-r28-eof-refs-20261124-01` | R28 基准 SDK 在 `sdk/native/use/install/lib`，库 SHA-256 见可机读片单。`full_subset.py` 做扩展语法检查。 |
| `$OUT/vvenc-r28-motion-geometry-20261124-01` | `intranet-inputs.json` 记录手指小样来源，`measure_blocks.py` 示范用对应的 `DecoderApp-cu` 导出选块。需要判断杂色位置落在哪个 CU、是帧内还是帧间时，可将导出的块与同一显示帧坐标对齐。 |
| `$OUT/vvenc-r34-inter-search-20261125-01`，下称 `LAB` | `bi_panel.py` 选择实际码率匹配的流并检查画质。`cast_scan.cpp` 扫偏绿。`bi_window_oracles.py` 驱动全片子集及双解码。每次新构建使用独立标签和输出目录，旧标签会复用回执。 |
| `$LAB/bi-panel/<tag>/panel-{quality,live}.json` | 每行有 `score`、码流 SHA、码率差、四项最低值、色差和色偏结果。沿该行的 `score` 找真实配对，部分位于 `rate-matches/`。评分目录的 `frames.csv` 是逐帧 PSNR。码流目录的 `frames.csv` 是逐 AU 时间戳台账。 |
| `$LAB/bi-window-oracle/<tag>/audited.json` | 每个完成组合的码流 SHA、帧数、解码像素 SHA、`gate.json`、`full.json`。核对 SDK 与 SHA 后再计入完成数。 |
| `$LAB/quality-stage-candidate/visual-review/` | 手指连续帧 `whole-hand-12-15.png`、`ch09-color-bias.png`、`05C-color-bias.png`、`game12-100-color-bias.png` 和测量回执。另有 `$LAB/residual-color-review/` 的手背、手腕和字样图。 |
| `$LAB/bi-role-service/<tag>/` | `pixels/summary.json` 为实际服务输出的像素对照。`irregular/oracle/summary.json` 列实测 IDR 和分段检查。`$LAB/color-feedback-repeat/joint-residual/` 有 Live CH09 各次闭环重跑的选流和评分。 |

媒体、运行目录、VTM 探针和部分实验脚本单独保存在操作机。迁移时带上清单所指的源、码流、各码流旁的 `run.json` 与 AU 台账，以及下列探针和原始证据。52 条码流都有运行回执和 AU 台账，49 条旁边有 `profile.cfg`。另三条 R28 Quality 对照（13/P4、13/P8、ch14/P4）可按 `run.json.cfg_sha256` 从片单的 `profiles_by_sha256` 找到逐字节相同的配置。扩展检查器当前从 `/mnt/d/Code/ffmpeg_live/experiments/vvc-tool-screen` 读取 `summarize_tencent_structure.py`、`tencent-structure-counts-20261122.json`，从主工作区 `docs/evidence/vvenc-tencent` 读取 `replay.py`、`originals.json`、`prediction.json`、`fork-s4.json`。原机器的 `/var/tmp` 也存有门禁源，迁移前校验完整 SHA。

## 换一个新候选

1. 在实验机给新 SDK 单独起标签。核对 `$LAB/build/<tag>/source.json` 的 `library_sha256` 与 `/var/tmp/vvenc-r34-inter-search-20261125-01/<tag>/lib/libvvenc.so.1.14.0`。基准和候选的档位配置由 `study.py`、`closed_screen.py` 选择，闭环编码命令见上一轮的 `screen.py`，实际配置可按每条码流的 `run.json.cfg_sha256` 核对。库分支和提交从编码器仓库取得。[Nakiha/libvvenc_naki](https://github.com/Nakiha/libvvenc_naki)
2. 在 `LAB` 运行 `python3 -B bi_panel.py <tag>`。它按片交替检查 Quality 与 Live，先跑手指、游戏和 CH09，逐点写 `bi-panel/<tag>/panel.json`。某档触发平均画质或持续色块门槛后，该档停止。交付前逐项核对 13 个组合都有效配码，孤立色格要看连续帧。对 CH09 这类会随闭环重跑变化的点，分别留各次的码流和结果。
3. 对已配码的指定点运行 `python3 -B bi_window_oracles.py <tag> quality:finger:4`，后面可列多个 `档位:片名:并行度`。新候选用新标签。冻结码流可用下节命令在独立目录复核。交付时把所测提交、SDK 哈希、实际码流 SHA、画面结果、双解码覆盖和任意 IDR 的服务回执一起交给接手人。

## 在本机复核一个送测点

下例在 WSL Bash、父仓库根目录执行，默认取交付 Quality 的 `finger/P4`。修改前三个选择变量可复核清单里的其他组合。输出落入 D 盘的新目录。所需三个评分工具在 `/var/tmp` 的同名实验目录，其源码位置见上表。

```bash
set -euo pipefail
TIER=quality
CLIP=finger
P=4
REPO=$(pwd -P)
MANIFEST="$REPO/experiments/vvc-tool-screen/r34-quality-gate-sources.json"
OUT=/mnt/d/Code/ffmpeg_live/out
R28="$OUT/vvenc-r28-eof-refs-20261124-01"
TOOLS=/var/tmp/vvenc-r19-20261123-01/tools
WORK=$(mktemp -d "$OUT/quality-review.XXXXXX")
export TMPDIR="$WORK/tmp" PYTHONDONTWRITEBYTECODE=1
mkdir "$TMPDIR"

python3 - "$MANIFEST" "$TIER" "$CLIP" "$P" > "$WORK/inputs" <<'PY'
import json, sys
from pathlib import Path
m = json.loads(Path(sys.argv[1]).read_text())
tier, clip, slots = sys.argv[2:]
src = next(x for x in m["sources"] if x["id"] == clip)
case = next(x for x in m["cases"] if x["source"] == clip and x["max_parallel_frames"] == int(slots))[tier]
print(src["frames"], Path(m["paths"]["source_root"]) / src["path"], src["sha256"], sep="\n")
for item in (case["r28"], case["candidate"]):
    print(Path(m["paths"]["evidence_root"]) / item["path"], item["sha256"], sep="\n")
PY
mapfile -t v < "$WORK/inputs"
N=${v[0]}; SRC=${v[1]}; BASE=${v[3]}; ES=${v[5]}
printf '%s  %s\n' "${v[2]}" "$SRC" "${v[4]}" "$BASE" "${v[6]}" "$ES" | sha256sum -c -

# 优先读共享缓存。缺失时只在本次的临时目录解码。
get_raw() {
  local es=$1 role=$3 file="$OUT/vvenc-decoded-cache/$2.i420"
  if [[ ! -f "$file" ]]; then
    file="$TMPDIR/$role.i420"
    "$TOOLS/ffmpeg" -nostdin -v error -xerror -f vvc -threads 2 -i "$es" \
      -fps_mode passthrough -pix_fmt yuv420p -threads 1 -f rawvideo "$file" || return 1
  fi
  [[ $(stat -c %s "$file") == $((N * 777600)) ]] || return 1
  printf '%s\n' "$file"
}
trap 'rm -f -- "$TMPDIR/r28.i420" "$TMPDIR/candidate.i420"' EXIT
A=$(get_raw "$BASE" "${v[4]}" r28)
B=$(get_raw "$ES" "${v[6]}" candidate)
/var/tmp/vvenc-r29-kernel-trials-20261125-01/pair_scores "$SRC" "$A" "$B" "$N" "$WORK/frames.csv" > "$WORK/pair.json"
/var/tmp/vvenc-r30-search-budget-20261125-01/color_regions "$SRC" "$A" "$B" "$N" > "$WORK/regions.csv"
/var/tmp/vvenc-r34-inter-search-20261125-01/cast-scan "$SRC" "$A" "$B" "$N" > "$WORK/cast.json"

python3 "$REPO/experiments/vvc-tool-screen/check-r28-geometry.py" check "$ES" "$WORK/gate.json" \
  --structure "$TOOLS/DecoderApp-structure" --ffmpeg "$TOOLS/ffmpeg" \
  --trace-ffmpeg /usr/bin/ffmpeg --frames "$N" --bench-frames "$(dirname "$ES")/frames.csv"
python3 "$R28/full_subset.py" "$ES" "$WORK/full.json" --structure "$TOOLS/DecoderApp-structure" \
  --prediction "$TOOLS/DecoderApp-prediction" --ffmpeg "$TOOLS/ffmpeg" --trace /usr/bin/ffmpeg --frames "$N"
printf '%s\n' "$WORK"
```

读结果时先对齐 `gate.json` 与 `full.json` 的 `stream_sha256`、帧数、`passed` 及两套解码器的像素 SHA，再对 `frames.csv` 的坏帧和 `regions.csv` 的新增区域看片。`frames.csv` 的 `y29/cb29/cr29` 是评分器沿用的候选列名。扩展检查器使用实际码流目录中的台账时，保留它和压缩流的对应关系。

上述默认例子的核对值为 60 帧、实际码率差 −0.19685%、`min_guard` −0.11543 dB、新增严重格 0、持续格 0。候选码流 SHA-256 以 `baa6edc28f21` 开头，两套解码器的像素 SHA-256 都以 `3cea651875a3` 开头。

冻结参考表与无媒体回归可以独立复核。

```bash
python3 "$REPO/experiments/vvc-tool-screen/check-r28-geometry.py" verify-originals "$OUT/vvenc-r17-tencent-vtm-20261122-01/structure"
python3 "$REPO/experiments/vvc-tool-screen/test-r28-geometry.py"
```

## 片源、缓存及交付物的保留

- **优先保留**清单里的去重源、能还原源缩放及显示帧映射的输入和窗口记录、R28 / 候选压缩流、SDK 身份、配置、AU 台账、评分回执、结构 trace 与人工回看图。后续逐帧比较依赖同一输入。
- 共享解码缓存实际位于 `D:\Code\ffmpeg_live\out\vvenc-decoded-cache`，WSL 的 `/var/tmp/vvenc-r29-kernel-trials-20261125-01/picture-cache` 指向它。文件名是**压缩码流**的 SHA-256，内容是解出的 I420。先验证仍有同 SHA 的压缩流，再判断某项能否重建。部分历史解码结果尚缺独立压缩源，应保留。
- 本次逐文件核查在 `$OUT/vvenc-quality-handoff-20261125-01` 留有 `disk-summary.json`、`cache-stream-evidence.json` 和抽样重解回执。可借此区分正式源、重算缓存、人工对照副本和中断的临时输出。

**11 月 25 日的占用快照。** 以 18:46 的恢复记录为基准，约 21:10 扫描原始文件，21:22 读取卷剩余空间（香港时间）。D 盘共享缓存共 350 个完整 I420，占 54.60 GiB。其中 280 个、44.65 GiB 有逐个核对过 SHA 的压缩原流，抽取其中一条重解后像素 SHA 与缓存一致。这 280 个里有 278 个、44.56 GiB 在 18:46 后写入。此前单独保留的另外 70 个包括 20 个常用项和 50 个当时缺原流的项，后者本次只补证了 1 个。

同一时段 D 盘另写入 7 份服务回看源，占 1.52 GiB。逐字节哈希只对应片单中的 `11` 和 `13` 两份源。中断的 VTM 核验留下 `$LAB/bi-window-oracle-scratch/tmp7ti3nukd/vtm.yuv`，共 5.18 MiB。C 盘可读取的原生目录和 Ubuntu 根文件系统内未发现这一时段改写的 YUV。Ubuntu 内既有的 165.63 GiB 原始画面按逻辑文件大小计，其中 Q26 取证目录占 91.42 GiB。盘点范围为可读取路径中的常见原始视频扩展名，WSL 挂载的 D 盘只计一次。
