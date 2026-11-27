# R36 RC1 的 6348 短测

R36 RC1 沿用 R35 的服务、FFmpeg、Live/Quality 档位与码控，替换了 VVenC 的等价 CABAC 实现和与之配套的服务训练 PGO。运行包的 `release.json` 记录各模块哈希、Build ID、编码库 gitlink 与依赖键。独立的 Offline 用来测量剪枝，线上 RC1 包只加载发布库。

## 在 Xeon Gold 6348 上运行

在机器的本地盘解压 `r36-rc1-6348-ab.tar.gz`。需要 Linux x86-64-v3、Python 3、taskset，以及同一 socket 内可供当前进程使用的 8 个物理核。包内自带 R35/R36 库、服务、ffprobe 和片段 11/13。

```sh
cd r36-rc1-6348-ab
./bench-6348 --output /tmp/r36-6348-ab-01
```

默认跑 Live 片段 11 四槽和 Quality 片段 13 八槽。每项做两对顺序相反的 R35/RC1，逐项单路运行。每次启动一个隔离服务，使用预热 worker、8 个编码线程、固定同一组 CPU、30 fps 节拍与原档五秒码率上限。每段 180 帧，默认共送入 48 秒媒体。选 `--socket 1` 可以指定 socket，`--case live-13-p4` 可单测或重复指定。用 `--cross` 交叉运行两档、两片段、两种并行帧槽。输出目录须为新路径。

`paired.json` 记录每对及各对中位数，含 worker CPU 毫秒/帧的相对变化、稳态 P95 的毫秒差、实际视频字节变化、请求 QP 不同的帧数以及封装后逐 AU 载荷哈希是否相等。每项的 `result.json` 保留两个绝对值。P95 从第 61 个完成帧算到末帧，范围为服务输入包进入至实际 mux 写出/flush。脚本验证实际加载的 SDK、完整帧数、丢帧、五秒峰值码率和 strict-ten。主机、绑核、每帧记录和日志保存在同一输出目录。此短测用于单路 CPU 与时延，容量需要另测。

## 编码源码

- R36 RC1 VVenC 提交为 `e6841ea0dae8a273eb63e03fa7f1cbc892d21f9b`。[发布源码](https://github.com/Nakiha/libvvenc_naki/commit/e6841ea0dae8a273eb63e03fa7f1cbc892d21f9b)
- Offline 在 RC1 之上单独维护，提交为 `ad9591fd8b0e1acf6038532cca91a1b7211af85e`。档位可记录固定逐帧 QP 下的完整搜索结果，也可只重开一次合法搜索，比较其局部代价与整段输出。[使用说明](https://github.com/Nakiha/libvvenc_naki/blob/ad9591fd8b0e1acf6038532cca91a1b7211af85e/tools/live-offline-r36/README.md)
