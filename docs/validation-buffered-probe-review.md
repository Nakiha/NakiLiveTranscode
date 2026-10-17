# 第一批独立审查修复：缓存来源与探流预算

本文件保留第一批较早一轮实测证据。随后复审的对象/配置凭据失败恢复 P1/P2 已在 [失败恢复验证记录](validation-sdk-failure-recovery.md) 实测通过；第二批现状见 [帧策略验证记录](validation-frame-policy.md)。下文“第二批暂停”等是该轮历史范围，不代表当前阶段；不得把这里的旧 SDK 当作最新源码已通过。

保持第二批暂停。本轮只修第一批审查缺陷及相应测试，没有提交、推送、安装依赖、启动 Windows Python 管理器或绕过 VVenC/yyjson Git 元数据限制。使用既有 WSL `/usr/bin/python3` 和工具链，真实依赖构建最多 2 个任务；本轮日志在 `out/probe-review-fix-A7c4/`。

## P1：保留旧 mtime 后错误封存旧库

已复现，不只依据源码判断。`out/sdk-build-se08meer/observations.json` 使用真实 GCC/CMake/Make、微型真实共享库以及项目默认 `build-service.sh`→封存→skip 入口：

- FFmpeg 夹具 `libavformat/demux.c` 返回值从 1 改为 2，保留旧 mtime；默认构建仍运行出 1，新源码身份却封存成功，skip 也接受。
- VVenC 同样从 11 改为 12 后仍运行出 11。这不是 FFmpeg 专属问题，两个组件的时间戳对象缓存都需要绑定内容。

修复：每个组件具有源码/配方内容键，键变化或缺失时先清理该组件专用构建目录，再构建安装；只有成功后记录 `.naki-object-key`。schema 2 的封存必须核对两个成功构建凭据与 snapshot 一致，再记录安装字节。单有“编译前后源码摘要相同”不足以封存。

`tests/sdk_build_integration.py` 修复后端到端通过，证据 `out/sdk-build-vfwhtrm4/COMPLETE.json`：FFmpeg 默认重建及 skip 均运行出 `[2,11]`，再修改 VVenC 后默认重建及 skip 均运行出 `[2,12]`，mtime 保持不变。`tests/sdk_identity_test.py` 的 8 项检查通过。测试使用独立 `out/` 源码/缓存树，没有修改真实库源来注入标记。

这是保守的整组件失效策略，会增加本地依赖编辑后的编译成本；不删除源文件或历史归档，不承诺跨工具链的位级可重现。

## P2：分析预算绕过与中途 EIO

修复前证据 `out/probe-review-budget-tfpq7w_9/observations.json`：

- 音频-only 的 100ms 分析预算读完 8 秒、82,156 字节输入；缺 SPS/PPS 的 TS 同样读完 711,204 字节。
- 缺 SPS/PPS 原始 H.264 的 40ms 墙钟预算耗时约 7.31 秒，读完整个输入。
- 原始视频在第 8 次读取中注入 EIO 后返回成功。解析器排空产生可用画面，使只在外层保留错误的逻辑漏掉了这条路径。
- 同一夹具的音频 EIO、音频/视频取消原本已正确返回错误；原字节预算也有出口，不能把这些项写成原先全部失败。

修复：所有流都计入媒体分析预算，参数不全也不能跳过时长检查；在初始探流作用域内，raw/parser 重试循环独立检查 payload 字节、单调时间和取消。读取错误保留到最终返回，不能被参数成功覆盖。预算/取消不当成 EOF 排空部分 AU，后续正常读包前清除探流作用域，保留原 packet-buffer 所有权。

预算语义见 FFmpeg `doc/live-video-probe.txt`：没有可用视频时，时间耗尽 ETIMEDOUT、payload 字节耗尽 ENOBUFS；较早 EOF 仍是无效输入，实际读取错误优先。检查不能强行抢占单次协议/demux/decoder 调用，仍需要调用方 I/O/取消期限；不是任意驱动下的硬实时保证，也不是 wire 字节上限。

## 新增验收范围

- `probe_test.c` 为每个返回包记录 stream index、PTS/DTS、duration、flags、size 和 payload SHA256；独立比较跨音频/视频/数据的完整 payload 顺序，并检查晚私有 PES 的已知原始内容。
- 缺视频、缺 SPS/PPS、媒体/字节/墙钟预算，以及读取已推进后的 EIO/取消；另验证预算成功退出后，部分 AU 能继续读取而不丢包。
- AAC 多轨及 MP3 的完整包顺序，既有视频像素/时间戳、真实 RTMP/SRT 同连接晚音轨、取消/排空均重新通过。

## 本次实际构建与回归结果

真实 WSL 构建完成，日志 `out/probe-review-fix-A7c4/build.log`。安装 SDK schema 2 身份为 `5d289e4330d8e996f3b907fb4a2a63fb98515b7d0eb49dc1e9bf7c1159c89bdf`。源码仍为本地未提交修改：父仓基准 `e3343870fe9bfe219e6d69f20c19b93a90ed52da`，FFmpeg 基准 `f511375887a2226fd3971b6c2222785d264901e0`，分支均为 `codex/buffered-live-probe`。

本次重新执行通过：

- 探流完整验收：`out/probe-test-fzvovixh/COMPLETE.json`，包含 12 组 H.264/HEVC、8/10-bit、帧率/单帧 MP4/TS 对比，晚音轨/私有数据、MP3 完整包顺序、确定性视频选择，以及 9 个预算/读取错误/取消用例。40ms 墙钟用例观测约 42.10ms；这只是受控 AVIO 用例，不是生产延迟或容量结论。
- 预算成功退出后继续读取：探流位置为 32,768 / 644,957 字节，后续全部 200 帧及包顺序、像素、时间戳均与通用路径一致。
- 解码：`out/decoder-test-001x6q58/`；服务：`out/service-test-3x5uhj7p/`。
- VVenC PTS：`out/vvenc-pts-l_2w738h/`；实时复用：`out/live-mux-nte90cci/`；媒体钩子：`out/media-hooks-pn6ss8iq/`。各入口日志保存在本轮目录。
- 本次 `NAKI_SKIP_FFMPEG=1` 服务构建再次验证同一 SDK 后完成；`reuse-and-symbols.log` 记录结果。匹配 Build ID 的 DWARF 分别归档至本轮 `symbols/application`（13 个 ELF）、`symbols/ffmpeg`（6 个）、`symbols/vvenc`（1 个），同时保存 `sdk-manifest.json` 与 `source-record.json`。shell 语法检查、父仓及 FFmpeg `git diff --check` 通过。

没有新增 workflow/job；微型构建回归加入现有测试入口和证据导出。未运行 Docker 镜像构建、远程 CI、新的 sanitizer 或性能矩阵。本轮第一批修复待独立复审，第二批保持暂停。
