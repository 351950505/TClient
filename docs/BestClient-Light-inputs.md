# BestClient-Light 快速输入权威适配

## 权威来源与范围

- 行为权威：BestClient-Light `main`，固定提交 `c56daba94b2b0ff7b5590a16ca463d9ff5182b98`。
- 对照标签：`v5.0`，提交 `7ff5a2ae0569cac23c5d29f8dd7ef0ff8aaa5a8f`。本次涉及的输入枚举、配置、预测、冻结和渲染关键文件与上述 `main` 提交一致。
- TClient 基线：`release/v1.7.8.72`，提交 `aa7ed5091051a3cf620bebf07134d92c1b63e3d3`，并保留本工作树中已经验证的更新镜像和渲染修正。
- 许可：MIT，Copyright (c) 2026 BestProject。完整许可见 `other/licenses/BestClient-Light-MIT.txt`，更新包副本见 `data/tclient/licenses/BestClient-Light-MIT.txt`。

本适配以 BestClient-Light 为双方重叠模式的算法和控制流权威，但不整文件复制上游客户端。TClient 继续保留现有配置键、模式编号、双号缓存、Dummy 输入、Remove Anti、Ghost/Unpred 渲染和独有模式。

未引入 BestClient-Light 的 SnapTap、Sub-Tick aiming、Auto margin 等非模式功能。

## 模式映射

| TClient `tc_fast_input_mode` | TClient 名称 | BestClient-Light 权威模式 | 处理 |
|---:|---|---|---|
| 0 | Tater | Fast | 算法和预测时间线以上游 Fast 为准 |
| 1 | Meow | — | TClient 独有，保持不变 |
| 2 | Snow | — | TClient 独有，保持不变 |
| 3 | Next | — | TClient 独有，保持不变 |
| 4 | 保留空洞 | — | 继续无效，不复用编号 |
| 5 | Salatik | — | TClient 独有，保持不变 |
| 6 | Ancient | — | 原 TClient Ancient，保持不变 |
| 7 | F | F | 算法以上游 F 为准 |
| 8 | Best | Best | 算法和 Others 调度以上游 Best 为准 |
| 9 | Saiko | Saiko | 算法和 Others 调度以上游 Saiko 为准 |
| 10 | Delta | Delta | 算法和 Others 调度以上游 Delta 为准 |
| 11 | Cloud | Cloud | 完整 snapshot/PreInput 时间线以上游 Cloud 为准 |
| 12 | Light | Light | 算法、钩连和 snapshot 时间线以上游 Light 为准 |
| 13 | Azure | Azure | 算法和 snapshot 时间线以上游 Azure 为准 |
| 14 | BestClient-Light Ancient | Ancient | 算法、稳定器和安全保护以上游 Ancient 为准 |

模式编号、现有 `tc_*` 配置键、范围、默认值和设置界面顺序均不改变。

## 权威算法

- **Fast / Tater（0）**：预测量按 `amount / 20` tick；正量预测上界取 `ceil`；环形预测槽必须精确匹配 `Tick-1` 和 `Tick`。
- **Best（8）**：有效偏移为 `amount / 100 × (1 - smoothing / 200) × (1 + latency / 100)`；插值模式 1 为线性、2 为 `3t² - 2t³`、3 为按位移距离混合。使用 `tc_best_input_interpolation`，不借用 Salatik 配置。
- **Saiko / Delta（9 / 10）**：偏移按 `amount / 100` tick；与 Best 一样，Others 走 ImmediateOthers，直接使用各自快速位置。
- **F（7）**：偏移按 `amount / 1000` tick，最多使用 2 个完整 tick 的安全外推。残差平滑使用上游 144 Hz 参考指数算法；TClient 将上游函数静态状态改为每客户端可重置状态，避免 ClientId 复用旧残差。
- **Cloud（11）**：本地预测上界为 `ceil(amount / 100)`；Others 固定 1 tick，但只有当前物理连接收到服务器 `PreInput` 后才启用。当前 tick 与未来 tick 都严格核对环形槽。未来槽无效时保留未偏移当前 tick 的严格插值结果。
- **Light（12）**：普通量按 `amount / 100` tick；钩连量按毫秒参数或兼容换算。钩连对象即使关闭普通 Others，也可按上游规则使用对应钩偏移和预测上界。
- **Azure（13）**：本地预测上界为 `ceil(amount + 1)`，Others 为 `ceil(amount)`；实际采样偏移仍为 amount，不额外增加一个 tick。
- **BestClient-Light Ancient（14）**：按真实 `GameTickSpeed()` 将毫秒拆为整 tick 与小数；方向反转且垂直速度占优时目标量乘 `0.55`，下降使用 `0.70`、上升使用 `0.35` 的稳定器。Freeze/Hook 状态变化时回退 regular 位置，并限制额外位移。

## 预测、冻结和渲染时间线

- Snapshot 模式精确为 Cloud、Light、Azure、BestClient-Light Ancient（11–14）。进入 snapshot 模式时不生成旧 AntiPing smooth 窗口；Others 关闭后使旧 Improved 缓存失效。
- Best、Saiko、Delta（8–10）的远端 Others 使用 `ImmediateOthers()`，在渲染和冻结路径中直接取权威快速位置。
- Cloud 的远端冻结路径不会像 Light/Azure/Ancient 一样直接跳到远端时钟；它保留冻结基线，再在该基线上应用 Cloud snapshot 偏移。
- Snapshot Improved 只执行 snapshot offset 和严格槽采样；不调用完整最终位置函数，避免提前穿过 Ancient 的状态保护和位移上限。
- Saiko 的超前预测 tick 不合成 TClient 通用 Dummy 快速输入；其他 TClient Dummy/复制/锤击输入路径保持原有行为。
- 预测结束后恢复 regular world；Ghost 和 Unpred 仍是渲染末级覆盖。
- Light 钩连位置仍可在普通 Others 上界之外保存；Cloud、Light、Azure 的 snapshot 读取使用严格槽。Ancient 最终 `SamplePos` 保留上游较宽范围检查、状态保护和位移限幅，不被 Cloud 的严格槽实现覆盖。

## TClient 平台适配

以下差异是有意兼容层，不是偏离权威算法：

- 上游单连接的 `m_ReceivedPreInput` 在 TClient 中按主连接/分身连接分别追踪；每个玩家的 PreInput ring 也按物理连接隔离，预测只读取当前活动连接。Dummy 断开会清分身连接能力、ring 和快输状态，切号不交换物理连接能力。
- F 的时间平滑状态按客户端、按物理连接保存，并在 `CClientData::Reset()` 或对应 Dummy 断开时清除，而不是使用上游函数内静态数组。
- 任意快速输入模式变化都会触发一次立即重预测；模式稳定后不会重复触发，防止 Cloud 等独立缓存模式在切换后一帧消费旧 ring。
- 保留 `tc_fast_input_mode` 编号、模式 4 空洞、Meow 旧键迁移、每模式 × 双号输入缓存，以及 `GetDummyFastInput()` 的 TClient 控制逻辑。
- 保留独有模式 Meow、Snow、Next、Salatik、原 Ancient 的既有行为。
- 保留 Remove Anti、AntiPing、Ghost、Unpred、Light hook-linked 存储，以及 TClient 的冻结外观适配。
- 不引入 SnapTap 和其他不属于本次重叠模式同步范围的上游功能。

## 验证

### 权威隔离夹具

运行：

```text
python -X utf8 -I scripts/test_bestclient_light_inputs.py
python -X utf8 -I scripts/test_remove_anti_fastinput.py
python -X utf8 -I scripts/test_fastinput_lifecycle.py
```

当前结果：

- `test_bestclient_light_inputs.py`：`337 / 337` 项通过。覆盖模式 0、7–14 的偏移、Best 三种插值、8–10 ImmediateOthers、精确环形槽、F 范围地平线、Cloud PreInput/fallback、Light hook 上界、Azure 和 Ancient 安全保护、229 项 snapshot 渲染/冻结/切换矩阵、Fast → Cloud/Cloud → Best 的立即重预测，以及主/分连接动作锁存隔离。
- `test_remove_anti_fastinput.py`：84 个场景、`252 / 252` 项断言通过。覆盖 14 个有效模式 × 3 个冻结阶段 × 2 种平滑窗口 × 本地/Others，并固定执行数量，禁止 inactive `continue` 造成假绿。
- `test_fastinput_lifecycle.py`：`17 / 17` 项通过。覆盖 dummy 早退前的 PreInput 登记、主/分连接 ring 隔离、活动连接读取、全量 reset、F 每连接状态、任意模式切换重预测、动作锁存按连接保存，以及 Dummy 断开时普通输入/快输/Ancient/PreInput/F 的完整清理。
- 对历史 `HEAD` 运行 Remove Anti 夹具会明确失败 60 项，不再静默跳过未激活模式。

### C++ 纯算法测试

`src/test/tclient_fastinput_test.cpp` 固定上游数值向量，覆盖：

1. 正量预测 tick 的 `ceil` 边界。
2. Best 有效偏移。
3. Best 线性、三次和距离混合插值。
4. F 在 60 Hz 与 144 Hz 调用频率下的帧率无关收敛。

当前 `TcFastInputAuthority.*` 为 `4 / 4` 通过。

### 验证边界

隔离夹具从生产源码提取方法并用 MSVC 临时编译；它能验证固定输入和控制分支，但不能替代完整游戏网络时序。最终还必须通过 Release `game-client`、`TClientUpdater`、完整 `testrunner`、专项回归和打包校验。

真人操作及主/分身双客户端联网手感不在自动化环境覆盖范围内，交付时会单独标明。未提交、未推送、未发布，也不会覆盖玩家运行目录。
