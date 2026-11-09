# BestClient-Light 快速输入适配

## 来源与范围

- 上游发布：`v5.0`，固定源码提交：`7ff5a2ae0569cac23c5d29f8dd7ef0ff8aaa5a8f`。三个模式实现与 main 的 `787896f08e34532ad243372f8f8443800be5e58f` 对照文件逐字节一致。
- 本地基线：TClient 1.7.8.66，`1740dfd2717863ef9c16ff02361ec30835ad7b68`。
- 参考 `src/game/client/components/bestclient-light/{light,azure,ancient}_input.cpp` 和 `src/game/client/gameclient.cpp`。
- 许可：MIT，Copyright (c) 2026 BestProject。完整许可见 `other/licenses/BestClient-Light-MIT.txt`，并在 `data/tclient/licenses/BestClient-Light-MIT.txt` 放置随更新包分发的副本。

用户确认保留原 Ancient，新增独立的 BestClient-Light Ancient。现有模式编号、默认选择和配置键不变；未移植上游独立的 SnapTap 或其他功能。

## 配置

新增模式均默认预测量为 0，不会自动开启；使用现有总开关 `tc_fast_input`。

| tc_fast_input_mode | 模式 | 量 | 其他选项 |
|---|---|---|---|
| 12 | Light | `tc_light_input`，0–500，百分之一 tick | `tc_light_input_others`、`tc_light_input_hook`、`tc_light_input_hook_amount`（1–100 ms，默认 37） |
| 13 | Azure | `tc_azure_input`，0–500，百分之一 tick | `tc_azure_input_others` |
| 14 | BestClient-Light Ancient | `tc_light_ancient_input`，0–100 ms | `tc_light_ancient_input_others`、`tc_light_ancient_input_others_amount`（0–100 ms，不超过自身量） |

设置入口沿用现有 Fast input mode 下拉菜单。Light、Azure、Ancient 等模式名保留英文，其他文案为中文。

## 适配要点

- Light：复用现有 Snow 钩子关系检测；独立参数及输入缓存。钩连对象在 Others 关闭时仍可使用对应钩偏移。过量预测期间只为符合条件的其他玩家保存位置。
- Azure：本地预测上界为 `ceil(amount + 1)`，Others 为 `ceil(amount)`；采样偏移仍为 amount，不额外加一个 tick。位置缓存严格核对 tick。
- BestClient-Light Ancient：按 `GameTickSpeed()` 换算毫秒；独立方向反转稳定器。普通快速位置采样保留上游范围检查，冻结时间线采样使用严格 tick 检查。局部和远端快速位置均检查 FreezeEnd、HookState、HookedPlayer，并限制额外位移；其他玩家的后处理以 regular 位置为基准。
- 保留 Remove Anti 的冻结时间线，而非无条件替换为普通快速位置。
- 新模式的 Others 启用时保留预测角度，避免位置超前但角度回退到旧快照。
- `tc_remove_anti` 冻结路径先调整基础时间线，再叠加所选快速输入偏移；涵盖本地与 Others。修复 Meow 时间窗跳过采样、Best/Saiko/Delta/Cloud 漏采样，以及 Next/Light/Azure/BestClient-Light Ancient 远端路径绕过冻结时间线的问题。
- 卡锤/卡钩冻结高亮增加身体和脚部独立 HSLA 颜色设置；设置状态齿轮增加 HSLA 颜色设置。默认配色保持原样。
- 进入新模式时重新释放动作方向锁存并触发重预测；重置时清空新增输入缓存。
- 沿用本客户端原有方向输入管线，不引入上游 SnapTap；因此本适配不是整个 BestClient-Light 输入系统的逐字复制。

## 验证

运行 `python -X utf8 -I scripts/test_bestclient_light_inputs.py`。该测试从生产源码提取方法，模拟时钟、快照和输入接口；使用 MSVC 在临时目录编译，不能代替完整游戏运行。

6 组通过：

1. Light 本地、Others、钩连对象与陈旧位置缓存。
2. Azure 本地额外预测上界与 Others 小数偏移。
3. Ancient 不同 tickrate、Others 量上限、冻结/钩状态保护及位移限幅。
4. Remove Anti 冻结时间线及总开关关闭后的回退。
5. Light 方向锁存、Azure/Ancient 输入、Dummy、模式切换和重置。
6. 原 Tater/Ancient 配置独立，保留编号 4 的无效模式行为。

另外执行 `D:/kehuduan/perf-validation-20261109/freeze-fastinput-jka4d397/diagnose.py`，从当前源码提取冻结和输入位置方法，并使用引擎 `GetSmoothFreezeTick()`。对 14 种模式（保留空位 4），分别检查未冻结、冻结中、解冻恢复、Meow 平滑窗口开关，以及本地和 Others：修复前（v1.7.8.66 源码）132 个匹配场景中 59 个未叠加快速输入偏移；修复后 168 个新旧模式场景全部通过。该诊断是隔离夹具，不是联网游戏录制，也不模拟完整预测帧调度。

最终 `game-client` 与 `testrunner` Release 构建成功。重新构建的既有 C++ 测试 443 项通过，3 项原有禁用。翻译检查 494/494 通过，源侧与构建侧 TClient 简中词典已同步，`git diff --check` 通过。

尚未启动客户端做真人输入、双客户端预测或视觉验收；不声称手感与上游完全相同。未提交、未推送、未发布，也未替换用户运行目录的程序。
