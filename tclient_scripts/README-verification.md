# 本地自动验证入口

这是整体测试计划的第一、二阶段：统一调用已有安全测试、按改动选测、生成中文摘要与 JSON。**不会启动 DDNet、修改真实设置、启用 CI、推送或发布。** 下列命令仅说明参数；实际运行须由隐藏的后台进程树启动器执行，不能在前台终端/控制台直接运行测试或构建。

## 日常怎么用

从源码根目录运行（不需要安装新的 Python 包来使用调度器）：

```powershell
# 只看会测什么，不执行测试或构建
python -X utf8 -I -B tclient_scripts/verify_tclient.py plan

# 快速反馈：调度器/选测回归 + 词典门禁 + 源码接线与输入约束
python -X utf8 -I -B tclient_scripts/verify_tclient.py quick

# 已登记自动测试的完整验证：必须明确选择当前源码的已有构建目录
python -X utf8 -I -B tclient_scripts/verify_tclient.py full --build-dir build-vs2026-1795

# 桌面测试包 / 一次改动完成：预览并执行最小相关测试；可选传可信基线帮助精确比较
python -X utf8 -I -B tclient_scripts/verify_tclient.py plan --baseline C:/path/to/report.json
python -X utf8 -I -B tclient_scripts/verify_tclient.py affected --baseline C:/path/to/report.json --build-dir build-vs2026-1795

# 没有兼容的 full 基线时也可直接按 Git 差异进行有限选测，不会仅因缺基线跑 full
python -X utf8 -I -B tclient_scripts/verify_tclient.py affected --build-dir build-vs2026-1795

# 仅正式发布前运行 full
python -X utf8 -I -B tclient_scripts/verify_tclient.py full --build-dir build-vs2026-1795
```

构建目录名字只是本机例子；使用前核对其 `CMakeCache.txt` 属于当前源码。工具不自动新建构建配置或安装工具。构建复用缓存里的 CMake 路径，限制 `--parallel 2`；Visual Studio 还传 `/p:CL_MPCount=1` 和 `/nr:false`，关闭本轮 MSBuild 节点复用，避免构建退出后遗留工作进程。Windows 进程树统一设为 BelowNormal。

可用 `--repo` 明确源码根目录。报告默认写到系统临时目录，命令结束时打印路径；也可以用 `--output-dir` 指定**源码仓库外、尚不存在的新目录**，不会覆盖旧报告。

## 怎么看结果

构建验证限本地 Visual Studio Release/x64。请使用关闭了 `TEST_MYSQL` 的构建目录；验证器会在编译及运行原生测试前检查，不连接持久数据库。它从生成的 VS 项目读取产物路径，忽略父进程设置的 GTest 过滤和分片。本地 Cargo 离线，测试子进程的 Git 只允许 file 协议。这些限制不是通用网络沙箱。

- `summary.md`：中文摘要、每组状态、实际用例数、选中理由、未选项目和覆盖缺口。
- `report.json`：源码内容指纹（含未提交/未跟踪文件）、工具指纹、测试结果、产物哈希等机器可读证据。
- `*.log`：各执行步骤的日志；原生测试另有发现清单及 `native.xml`。
- 退出码 `0`：所选范围通过；`plan` 返回 0 仅表示计划生成成功，状态仍是 `NOT_RUN`。
- 退出码 `1`：测试失败或超时。
- 退出码 `2`：缺环境、测试未执行、结果不完整、输入变化等阻断。

**quick 通过不等于完整验证通过；full 也只代表当前注册清单，不代表全部玩法和画面都测过。** 这些边界会写进每份报告，不只写在说明里。

`full` 没有 `--build-dir` 时，其余安全测试仍可运行，但构建及原生测试必定是 `BLOCKED`，不会运行已有旧 EXE 来冒充本轮结果。

## 基线与选测

完整 `full` 通过报告可作路径比较基线，前提是报告自校验、仓库路径匹配且其文件指纹完整。基线必须是本工具生成的报告；它不是数字签名，不要手改后当成可信记录。基线只用于判断变更，不缓存或复用旧 PASS。

- 可信基线有效：比较报告内的文件快照与当前源码（含未提交、未跟踪、删除/重命名路径），按分组选最小相关测试。
- 缺少或不兼容基线：忽略该报告快照，只从当前 `HEAD` 的 Git 已跟踪差异、删除/重命名及未跟踪路径做有限选测。报告标记 `baseline_valid=false` 并列出人工范围确认；不把不可信快照或整个仓库冒充为本次改动。
- `plan`/`affected` 从不隐式升级到 `full`。公共核心/构建变更选择相关 build/native/runtime 组；测试接线变化选基础与相关接线组；未知/异常路径选择基础组并警告人工核对范围。若映射遗漏，人工按报告扩大范围或单独明确运行更多测试。
- `plan` 只预览，`affected` 执行所选组；二者都不产出完整基线，也不重用旧 PASS。缺基线、影响范围不确定不是自动全跑理由。
- 显式 `full` 选择整份当前 catalog，只有通过后才更新可信 full 基线。正式候选在 `release` 门禁中必须运行 `full`；用户明确提出 bug 并要求全量排查时也运行 full。任何失败、阻断或未执行均不得报为全量通过。
- 测试前后输入或构建配置变化、报告记录数低于基线：本轮结果无效或阻断，不静默放行。

## 当前清单与边界

登记在 `verification_catalog.py`，可选择逻辑在 `verification_policy.py`：

1. 调度器自身契约回归。
2. 纯选测策略回归。
3. 悬停说明源码/词典接线。
4. 地图进度源码接线。
5. 计分板邀请源码接线（不发送消息）。
6. 发布编排离线安全子集：4 个测试类，外部调用由原测试模拟。
7. stage 离线安全子集：3 个临时文件用例，不要求软链接权限。
8. 镜像解析/验签离线回归：需要已有 `cryptography`；不执行生产镜像测速。
9. 当前候选的真实客户端/testrunner 编译。
10. 当前构建的原生生产组件测试：发现清单与 XML 对照，保留 3 个明确已知的 ReplaceWords 禁用项。
11. 当前构建所绑定的 Rust 测试目标：通过 CMake `run_rust_tests`，强制 Cargo 离线，核对完整目标清单和实际逐例计数；当前清单固定 18 个文档用例和 6 个零用例目标，二者分开报告。
12. 全客户端简体 TCLocalize 词典覆盖检查；简体缺词阻断。繁体缺口和反向漂移按上游既定语义报告为非阻断提示，并结构化记录覆盖数。
13. 输入生命周期源码约束检查，报告实际检查条数；这不是客户端操作场景。
14. 快速缩放默认值、冻结/服务器快照边界源码检查；不是实际鼠标或在线实测。
15. 镜像候选排序纯内存回归；不执行当次镜像探测请求。

接线脚本的条目数不是游戏场景数，故报告不把“141 个文案条目”算作“141 个真实测试”。原生测试临时文件及数据副本仅供测试程序使用，放系统临时目录；不创建客户端 profile 或 `storage.cfg`，不运行客户端。

尚未接入：其他 C++/Node/Python 编译夹具、Rust standalone mastersrv 的服务场景、真实 GUI/在线/播放器、整包与实际更新安装验收，以及发布流水线门禁。词典扫描仅覆盖 `TCLocalize` 调用，不证明任意资源文字完整。报告会明确列出，**不把缺口隐藏成 PASS**。

新增测试要先检查网络、写文件、编译、签名或配置副作用，登记正确命令、工作目录、超时和非零执行证据，再加入选测映射。不要扫描到一个 `test_*` 就直接执行。

## 调度器回归

```powershell
python -X utf8 -I -B tclient_scripts/test_verify_tclient.py
python -X utf8 -I -B tclient_scripts/test_verification_policy.py
```

测试使用临时 Git 仓库和自建短命进程验证输入指纹、失败/超时、子进程回收、零用例、跳过、旧二进制阻断等契约，不操作用户仓库的提交历史或真实配置。

一次性成功日志用完按工作区清理规则处理；保留当前所需的通过基线、摘要和未结失败证据。没有发布/下载恢复核验时，不由本工具自动清理正式包或其他项目文件。
