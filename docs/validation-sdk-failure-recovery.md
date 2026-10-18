# 对象与配置缓存失败恢复修复：运行验收已通过

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

## 授权恢复后的实际结果（2026-10-17）

用户明确批准本项目必要 WSL 操作后，通过工具正式的按动作沙箱外审批进入原 Ubuntu，返回 `WSL_READ_OKPython 3.12.3`；没有安装依赖或更改系统安全设置。验收记录在 `out/sdk-recovery-acceptance-20261017T095859244/ACCEPTANCE.json`。

- shell 语法与 SDK 12 项单测通过。
- `out/sdk-build-pdjv17m5/COMPLETE.json`：成功路径、两库部分对象编译失败 / SIGTERM / SIGKILL、实际 seal 来源校验失败，以及 configure 改写配置后失败 / SIGTERM / SIGKILL，全部恢复旧内容和旧 mtime，验证默认重建、恢复前 skip 拒绝及恢复后 skip 成功。
- 实际默认 SDK/服务构建及 skip 再次校验通过；SDK 身份 `31772e67658363b9d03e17c1b04d9b6aa63355c3772dd9a7ccd669bd2767651b`。安装清单、统一对象凭据和配置键分别保存为该目录的 `sdk-manifest.json`、`object-cache-receipt.json`、`configure-key.txt`；FFmpeg 6 个、VVenC 1 个 ELF 的匹配 Build ID 符号独立归档在 `symbols/`。
- 首次夹具运行发现 Make 在 SIGTERM 时删除当前未完成目标，已改在第二个对象暂停，取证前一个已成功提交的 B 对象；configure 的旧 mtime 同样固定到 DrvFS 可保留的整秒。此前失败日志保留在 `out/sdk-build-0vcvst93/`、`out/sdk-build-rsi_8kyw/`。

下文的拒绝与未运行说明是修复过程的历史记录，已由以上实际验收补齐。第二批编码器/应用变更另行构建验收，不能沿用此 SDK 身份作为新库的通过证据。

本轮仅修复第一批的构建缓存凭据生命周期；第二批保持暂停。没有安装依赖、提权、绕过访问拒绝、提交或推送。

## 缺陷与实现

复审指出：成功构建 A 后，B 的部分对象已生成但后续编译失败，旧 `.naki-object-key=A` 仍存在。恢复 A 内容和旧 mtime 后，可能错误跳过清理并封存 B/A 混合对象。源码检查确认原脚本在构建前保留该旧凭据；本轮尚未实际复现。

修复采用整个 SDK 的原子提交凭据，而不是只依赖错误退出 trap：

1. 读取上次成功键到当前进程内存，随后在任何 configure/clean/build/install 前撤销统一缓存凭据、旧组件键、待封存键和安装清单。
2. 组件成功后只写 `.naki-object-key.pending`，它不能授权下一次构建复用；单独 VVenC 构建也只产生待封存结果。
3. seal 检查源码、两个待封存内容键、安装产物及工具信息，先写安装清单，最后原子写入 `.naki-object-keys.json`。只有这一步完成的键才授权复用，且必须与安装清单一致。直接调用 seal 也会先撤销先前的提交凭据和清单。
4. SIGKILL 在最后提交前发生时无需 trap 也没有可信对象凭据；正常错误、INT/TERM 另有退出清理。提交已经完成时，其键对应已通过全部封存校验的产物。

旧版组件凭据不会被导入，新配方需默认重建。上一轮 SDK `5d289e...` 和符号归档是旧配方证据，不能作为本轮源码已构建的证明。

## 后续配置凭据 P2

静态复审又指出 `.naki-configure-key` 的同类路径：A 配置成功后切 B，configure 已改写 `config.h` / `ffbuild/config.mak` 但失败或被中断，旧键 A 仍可能使恢复 A 的构建跳过 configure。

`build-ffmpeg.sh` 现在在判断需要重新配置后、执行清理或 configure 之前撤销旧键与临时键；configure 成功后先写同目录临时键，再原子重命名为可信键。中断发生在成功提交前时没有可信配置键，下次必须重新配置；configure 已成功提交后的键可以跨后续编译失败保留，因为对应配置已完整生成。临时键由 EXIT 清理，SIGKILL 遗留的临时键不授权复用。配置存在性检查覆盖 `config.h`、`ffbuild/config.mak` 和 `Makefile`。

新增三个真实脚本夹具路径：configure 改写两份生成配置后返回失败、SIGTERM、SIGKILL。恢复 A 的 configure 内容和旧 mtime 后，检查旧键缺失、skip 拒绝、configure 调用记录确实新增一次 A，并验证两份配置及最终库运行结果回到 A，最后检查 skip。库包含分别来自 header 和 config.mak 的标记，避免只看退出码或键值漏掉遗留 B 配置。

同类静态检查覆盖 `scripts/build-service.sh`、`scripts/build-vvenc.sh`、两个解码器入口及其 Linux/MSYS2 构建脚本、Docker 构建入口和现有依赖 CI。服务/VVenC 每次在编译前调用 CMake，解码器脚本每次调用 configure，没有另一处自定义配置成功键绕过重新生成；CMakeCache 的符号归档只是复制，不充当成功凭据。Docker/CI 使用上述构建入口。本轮未改动这些入口或子模块，也未扩大为工具链/CMake 缓存重构。以上为静态结论，不代表已实测这些工具的中断恢复。

## 新增回归与当前状态

`tests/sdk_build_integration.py` 继续使用隔离目录、真实 GCC/CMake/Make、真实 DSO 和默认脚本，新增：

- FFmpeg / VVenC 各自先编出 B 标记对象，再因另一个真实 C 文件语法错误失败。
- 两库各自在首个 B 对象编译完成后注入 SIGTERM 和 SIGKILL；独立链接并运行部分对象，确认确实生成了 B。
- 两库真实安装后由夹具改动源码，使实际 seal 来源校验失败。
- 每条失败路径均检查可信凭据及安装清单缺失，恢复 A 内容及旧 mtime 后先验证 skip 拒绝，再默认构建并验证运行值回到 A，最后验证 skip 成功。

`tests/sdk_identity_test.py` 新增待封存/旧组件键不得授权复用、统一凭据必须匹配安装清单、撤销清除所有凭据、直接 seal 校验失败撤销先前提交四项检查。

**尚未运行本轮单测、真实失败恢复、shell 语法检查或实际 SDK 重建。** 首次只读命令 `wsl -d Ubuntu -- /bin/bash -c 'printf WSL_READ_OK; /usr/bin/python3 --version; command -v gcc; command -v cmake'` 返回退出码 1 和 `Wsl/E_ACCESSDENIED`，未进入 Linux 命令。遵守委托限制，未提权重试，也未改用其他路径或 Windows Python。原生 `git diff --check --ignore-submodules=all` 通过；它不能替代运行验收。

配置 P2 这一轮按明确限制没有再次调用 WSL、提权或使用替代执行工具；三个 configure 恢复用例同样标记未运行。

当时约定的最小验收顺序是现有 WSL Python 单测、真实微型失败恢复夹具，再通过现有默认服务构建与 skip 入口验证真实 SDK；现均已完成，证据见本文顶部。
