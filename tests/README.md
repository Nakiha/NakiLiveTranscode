# 测试导航

统一入口是 scripts/test-service.sh，清单在 service_suites.json。支持 quick/full/sanitize 与重复 --suite ID；native 代表 CTest quick 集合。详细依赖、证据与覆盖范围见 [测试说明](../docs/testing.md)。

unit 是 C 模块测试；integration 是文件/实流/进程完整链路；fixtures 是样本、探针与故障注入；support 是可复用工具；checks 是入口、依赖及组织边界检查；benchmarks 是独立测量。

不要通过导入另一个 integration 套件复用工具。公共支持层导入时不得创建目录、选择端口或启动进程；服务启动器接收调用套件自己的结果目录。添加完整套件时更新清单，添加 C fixture 时更新 CMake；Docker 按源码类别复制，入口检查验证构建输入闭包。

性能入口是 scripts/bench-service.sh，见 [测量边界](../docs/benchmarking.md)。短时功能测试和测量不替代目标机器容量、真实网络与长时稳定性验收。
