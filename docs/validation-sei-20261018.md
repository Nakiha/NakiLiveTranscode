# SEI 类型选择透传本地验证（2026-10-18）

本轮用户确认：模板用 `uuid`/数字选择类型；type 5 的 payload 无需符合 UUID 布局；跨 H.264/HEVC→H.264/VVC 保留 type/payload、按目标格式重封装；携带选中消息的画面不允许过载抽帧。接口及输入结构/预算边界见 [SEI 合同](sei.md)。

基线父提交 `22a26999c9e97f07ff71cabf9ba7539c3e340907`。FFmpeg `4bee779b3dd59407a07baa8d6fc386cf95d8398b`、VVenC `29e3dc57a91d03833ead6370f5cd03f6d61242a5`，未修改子模块和 SDK 配方。本轮是父仓本地工作树实现，未提交、推送、发布或部署。既有未跟踪 `next-metadata-batch-and-release.md` 保持原样，它记录的是旧规划。

## 最终 SEI 专项

`out/sei-bg_l4j2z/COMPLETE.json`：27 项全部通过，运行前后 `app/*.[ch]`、CMake 和 `tests/sei*` 源文件哈希一致，完整记录在同一 JSON。构建入口使用 `scripts/build-service.sh`、已核实匹配的本地 SDK、两个编译任务；后续增量构建沿用该 CMake 构建目录。最终源码构建日志 `out/sei-temporal-build.log`。

- H.264/HEVC 长度前缀输入，分别输出 H.264/VVC × TS/MP4/FLV，12 项；每项都有缩放与水印。
- H.264/HEVC Annex B TS 输入，2 项。
- 四个真实输出分别选择 uuid、100/101、300、空列表，独立字节/PTS/计数比对；使用按 DTS 节奏回放，避免把不限速文件灌入的分支压力当成 SEI 失败。
- HEVC 首张画面即含短 UUID、VVC 1–5 帧尾组、controller 强杀后同 worker 接管、显式停止、hook 失败、消息数预算超限。
- 稀疏 SEI 过载保护与每帧 SEI 的全保护；实际分支选择结果、输出包和消息均对账。

独立 C fixture 直接序列化消息，不调用应用 SEI 写入器；独立输出提取器检查实际封装包中的消息 type/payload、顺序、重复及对应 PTS。使用 1 字节 type 5、两个相同 type 100、type 101、type 300 和 300 字节 payload，覆盖连续 255 编码和 emulation prevention。HEVC prefix/suffix 到 VVC 的位置保留，VVC 注入 NAL 的 temporal_id_plus1 与实际输出 VCL 匹配；H.264 无 suffix 类型，映射到 VCL 前并保留消息顺序。

标准 VVC 解码器拒绝非标准短 type 5 的现象已实际复现。因此不能把这批文件描述为任意标准播放器直接可播；验收先核对原始输出 SEI 字节，再由独立解码探针移除 SEI NAL、保留包 PTS 并解码，核对所有画面及展示顺序。该探针只是测试工具，服务输出不会删除这些选中消息。

## 单元及内存检查

`out/bin/naki-sei-test`、`out/bin/naki-control-spec-test` 通过。覆盖短/空 payload、重复消息、部分选择、H.264/HEVC/VVC framing、length-prefixed 输入、长 type、suffix、temporal id、非法外层长度/尾部、多画面/未绑定 AU 拒绝、64 消息上限、65536 字节可接受及 65537 字节拒绝。配置指纹验证 uuid/5 别名与类型顺序归一化，且改变有效选择会改变意图；分支继承及空列表覆盖通过。

最终 ASan + UBSan + LeakSanitizer 证据：`out/sei-sanitize-confirmed-7VDa9F/test.log`，`complete.exit=0`。这是独立 SEI 解析/重封装和配置单测的消毒器检查；FFmpeg/整个服务未重新编译为 sanitizer 版本，不据此宣称整个媒体栈无泄漏/竞态。

## 原有功能回归与检查范围

`out/sei-regression-JKiYaS/` 中 media_hooks、multi_output、frame_policy、vvenc_pts、watermark、service 六套真实集成回归全部通过。它们运行于 `out/sei-xit0u4re/COMPLETE.json` 对应的功能源码；随后唯一媒体逻辑补充是 SEI 重封装 NAL 的 temporal id 选择，最终 temporal id 修改后重新执行上述 SEI 单元、配置单元、27 项 SEI 集成及 sanitizer；没有重复全套原有回归。未启用 SEI 的原有路径不进入该重封装逻辑。OpenAPI JSON/所有本地引用和 `git diff --check` 通过。

本轮未执行 SDK 重建、远端 CI/Docker、生产接收端、长稳或容量测量；不新增时延/吞吐/SLO 结论。未覆盖任意分场、多层、多个画面同包或 SEI-only 跨包归属，当前对此明确拒绝。没有增加 SEI 运行中更新、客户响度算法或 SCTE-35。
