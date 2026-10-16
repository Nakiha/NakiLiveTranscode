# FFmpeg 解码实验归档

`ffmpeg-7.0.3/` 是公开可获取的固定提交子模块；其他目录只保留在本机。

- `results-load/report.html`：API 1 固定线程对照矩阵。
- `results-auto/report.html`：API 2 自动模式报告。
- `results-*-baseline/`：原版输出及 golden，必须保留。
- `fixtures-load/`、`fixtures-auto/`：冻结的合成测试媒体。
- `install-*/`：历史各版本独立库与探针。
- `build-*/`、`.plot-venv/`：旧缓存，仅归档。
- `delivery/`：原交付包、SHA256 与导出数据。
- 根级 `.log`、`.py`、`.sh`：原日志与一次性脚本，后者含原路径，仅归档。

新克隆的冻结报告在子模块 `tools/live-async/validation/` 中。媒体不包含于 Git；本机完整包为 `delivery/ffmpeg-7.0.3-live-auto-3824929-offline.zip`。向新机器转移完整包时，按包内 README 在独立目录展开复测；若要使用本仓库的测试入口，应将包内 `results-msys2-baseline/`、`results-linux-baseline/` 等冻结数据恢复到本目录，不覆盖子模块源码。缺少对应数据时不能宣称已复现历史结果。
