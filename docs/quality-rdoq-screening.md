# Optional RDOQ quality profiles

The experimental profiles `live_3b_2s_rdoq2` and `live_3b_2s_rdoq2_sel2` restore RDOQ on the existing `live_3b_2s_speed` search settings. They require CQF or constrained CRF and leave the default live profile unchanged.

| Profile | RDOQ | SelectiveRDOQ | ALF / CCALF |
| --- | ---: | ---: | ---: |
| `live_3b_2s_speed` | 0 | 0 | 0 / 0 |
| `live_3b_2s_rdoq2` | 2 | 0 | 0 / 0 |
| `live_3b_2s_rdoq2_sel2` | 2 | 2 | 0 / 0 |

RDOQ mode 2 is the existing faster preset's quantizer mode. The first candidate applies it without selective gating; the second enables the fork's natural-content selection (`SelectiveRDOQ=2`). RDOQTS remains separately enabled by the preset. These profiles retain QT16/chroma8, SearchRange64, MinSearchWindow32, FastIntraTools2, QPA, SAO, past-only MCTF, source-I/3B behavior, BIM=0, and the configured future-frame bound. Preset, threads, rate control and timestamps are not overridden by these tool switches.

Quality screening must compare actual output bitrate, exact selected PTS, all-frame PSNR/VMAF and concrete decoded pictures on more than one content sample. Equal CRF is an initial observation, not proof of rate-matched improvement. Record CPU, throughput and service ingress-to-actual-mux latency as costs; a quality-stage candidate may be slower than real time. A later performance stage must freeze the selected quality target before changing execution cost. These optional profiles alone establish no measured quality or live-capacity claim.
