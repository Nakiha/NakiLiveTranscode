# 工作区整理（2026-10-16）

历史迁移记录：以下版本、性能、路径和“下一步”仅描述 2026-10-16 的窗口。当前是 n9.0.2 底座的 Linux 单输入/固定多输出服务，实际合同与未验证项见[文档索引](README.md)、[当前验证](validation-core-refactor.md)。原迁移数据与 n7 冻结测量不改写。

原聊天：`codex://threads/01a1407b-de64-7662-a6ff-c008ef6315c0`，标题「分析 FFmpeg 7 解码延迟」。

## 版本与证据

| 提交 | 内容 |
| --- | --- |
| `199fc48` | 原版 Windows/Linux 基线工具 |
| `665d09b` | API 1：异步提前交付、非阻塞接口 |
| `086e3e7` | API 2：压力调度与协作预算 |
| `3824929` | 本次固定的最终工具版本 |

聊天及冻结报告记录了原版/改造版的逐帧哈希、顺序、时间戳、生命周期、预算及 ASan 检查。固定线程矩阵为 1,056 次运行。TSan 在原 WSL 环境未完成。这些是历史证据，本次整理不等于重新完成性能或生产验收。

自动模式保留预建 worker，调整的是允许在途的任务数。解码测量不包含网络、解封装初始化、filter 和编码，完整直播转码及连续规格切换仍待验证。

## 本机文件迁移

原根目录所有文件原样移入 `experiments/decoder-20261016/`，保留相邻布局与所有版本，不删除产物。该目录中的 `reorganization-inventory.local.json` 记录原路径和新路径，不提交。

FFmpeg 作为子模块固定提交；其源码与冻结报告不改写。根仓库仅跟踪项目文档、入口脚本及子模块指针。媒体、日志、原生库、虚拟环境、离线 ZIP 不上传。

旧编译目录、绘图虚拟环境和根级打包/捕获脚本仅作历史归档：可能含绝对路径，不应直接运行。旧构建缓存不复用；新入口强制使用 `out/`。历史结果内记录的旧路径继续保留以保护溯源信息。

原目录到新目录示例：

| 原路径 | 新路径 |
| --- | --- |
| `ffmpeg-7.0.3/` | `experiments/decoder-20261016/ffmpeg-7.0.3/` |
| `results-auto/report.html` | `experiments/decoder-20261016/results-auto/report.html` |
| `results-load/report.html` | `experiments/decoder-20261016/results-load/report.html` |
| `delivery/` | `experiments/decoder-20261016/delivery/` |

Linux 的旧对象缓存位于 WSL `~/.cache/ffmpeg-live/`，本次未移动。它们也可能引用旧路径，后续使用新构建入口。

## 下一步

后续已加入 Linux 单路 C 服务与完整链路追踪，见 [服务说明](service.md)。在线规格切换和多路 CPU 竞争仍待实现/验收。底层改动继续在 FFmpeg fork 提交，再由主仓库更新子模块指针。

## 整理后的检查

2026-10-16 在本机完成：65 个原始顶层条目迁移核对；四个带 SHA256 文件的交付 ZIP 校验；文档链接及脚本语法检查；新目录 Windows/MSYS2 完整解码构建；自动模式异步回归通过，包括逐帧哈希/时间戳与冻结原版一致、排空/flush、反压、坏包恢复、关闭、分辨率切换及 10-bit。

本地日志：`out/build-windows.log`、`out/test-windows.log`；逐帧记录：`out/results/windows-auto-01YsCnwG/`。坏包用例中的解码错误日志为预期输入。本次未重新运行 Linux 构建、ASan、共享预算专项或完整性能矩阵，不据此更新性能结论。
