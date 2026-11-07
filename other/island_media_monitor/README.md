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

## 相关源码

- 游戏侧集成与数据源优先级：`src/game/client/components/tclient/island_music.cpp`
- 第三方 netease-watcher v0.7.1：https://github.com/YUCLing/netease-watcher （发布包 `netease-watcher-no-tui.zip` 内 `netease-watcher.exe` SHA-256 `1be5313339c596f15d93f51b02abd7556f9e9feea84dd884c9f63ce1f3617f09`、`wndhok.dll` SHA-256 `08a7f97927e949cacfb3be7d495cfa077a8f8d04bf77142b1a5aacdbdfa9654e`，与 TClient 各发布包所载逐字节一致，未修改）
