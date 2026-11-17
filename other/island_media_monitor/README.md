# island_media_monitor（灵动岛音乐媒体监控助手）

TClient 灵动岛音乐显示的伴生助手进程。由游戏客户端按需自动拉起，负责：

- 通过 Windows SMTC（GlobalSystemMediaTransportControlsSessionManager）读取系统媒体会话；
- 轮询网易云播放进度数据源（优先级：netease-watcher > CDP > LS 锚点），数据源经共享内存写给游戏客户端；
- 管理 `netease-watcher.exe`（第三方助手，须与本程序同目录）的生命周期。

## 当前发布状态

正式发布包**不随版本重新构建**本程序，而是沿用一份经审查的预构建二进制沿版传递（历史来源见 `客户端交接摘要.md` §180/§181/§185）。截至 v1.7.8.59：

- 发布包内 `island_media_monitor.exe`：237,056 B，SHA-256 `98873ec28bc47e26ce4793561914ff8194d2460781a74bcf868e968460444cf1`
- 该二进制与 .55/.56/.57 发布包逐字节相同。

## 从源码构建

单文件 C++，无第三方依赖（系统库 shfolder/ole32/MMDevAPI/WS2_32）：

```bat
cd other\island_media_monitor
b2.bat
```

`b2.bat` 调用 VS2026 的 vcvars64 后用 `cl /O2 /EHsc /DUNICODE` 编译，输出 `island_media_monitor.exe`（/SUBSYSTEM:WINDOWS）。旧 `build.bat` 用 vswhere 定位 VS，二选一。

注意：源码迁移入仓库后重新编译，产物哈希会因嵌入路径不同而变化；下一次发版若决定改用 freshly-built 产物，需同步更新打包脚本 `prepare_release_stage.py --monitor` 指向与 stage-inventory 校验。

## 2026-11-17 审查修复（随下一版发布）

源码已修复六项审查缺陷：leveldb 数字解析的堆越界读、WASAPI 激活回调超时后的 use-after-free、PID 表主线程/CDP 线程竞态、环回帧计数残留与 SHM 整块撕裂读、僵死实例无退出协议、LS 扫描 mtime 门控失效。下一版发布应改用本源码重建的 `island_media_monitor.exe`（spec 的 `monitor_path` 指向新产物并记入 stage inventory），不再沿用 .59 预构建件。夹具测试见 `test_island_media_monitor.cpp` 文件头；详细记录见 `D:/kehuduan/客户端交接摘要.md` §337。
该重建产物已随 v1.7.8.87 发布：238,080 B，SHA-256 `9d7cec4ccc78ad589c7a6b6e6de50b6efc55417231bc4f464ca7fb7c1769196d`。

## 2026-11-18 暂停信号兜底 + 轮询加密（待随下一版发布）

实测用户机器（C:\TClientLS_dbg.txt）：WASAPI 环回激活全部失败（`FAILED hr=0x80004005`）且新版网易云 LS playPostion 加密不可读（ppHit 仅 12 次）——原三层暂停判定在这类机器上**完全没有信号**，外部音乐软件点暂停后 monitor 永远报"播放中"，歌词一路走到曲尾。两项修复：

1. **watcher 冻结判暂停**（第 4 层）：环回/LS 都没有时，watcher 的 `time` 暂停冻结、播放前进，连续 ~3s 采样差在 ±150ms 内判暂停；仅在最近 5s 有成功轮询时累计（watcher 失联不算冻结）。
2. **watcher 轮询 1s → 300ms**：轮询间隔就是 seek 后位置收敛延迟上限（2026-11-18 用户报"拖动进度歌词有时不对"）。
3. **环回激活连续失败退避**：环回不可用的机器上，失败流每 50ms 主循环立刻重生 = 每秒 20 次完整激活尝试（线程/COM/日志 churn），纯烧 CPU（玩家报"比其他客户端卡"的元凶之一）。连续 3 次失败后该 PID 退避 60s，之后每 60s 探一次；网易云换 PID 清记录，流存活清计数。

产物：240,128 B，SHA-256 `4e06de18afa4e01f536e8e94035dee189786710039934913da9fdc0e839669df`。夹具测试 21/21 PASS（含既有全部用例）。发布时 `monitor_path` 指向本产物。

## 相关源码

- 游戏侧集成与数据源优先级：`src/game/client/components/tclient/island_music.cpp`
- 第三方 netease-watcher v0.7.1：https://github.com/YUCLing/netease-watcher （发布包 `netease-watcher-no-tui.zip` 内 `netease-watcher.exe` SHA-256 `1be5313339c596f15d93f51b02abd7556f9e9feea84dd884c9f63ce1f3617f09`、`wndhok.dll` SHA-256 `08a7f97927e949cacfb3be7d495cfa077a8f8d04bf77142b1a5aacdbdfa9654e`，与 TClient 各发布包所载逐字节一致，未修改）
