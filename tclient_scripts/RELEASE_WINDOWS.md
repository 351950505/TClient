# Windows 发布签名与打包

## 当前本地发布：每次打包重新测速（2026-11-11）

本地正式包统一通过 `release_orchestrator.py run --spec <spec.json>` 生成。阶段顺序为 `preflight → mirror_probe → build → stage → package`；需要上传时再显式使用 `--publish`。下文 CI 与 `.55` 的例子保留作历史签名说明，不能替代本节镜像步骤。

`mirror_probe` 每次从 `https://github.akams.cn/` 获取当次目录，使用客户端的 User-Agent、IPv4、TLS 校验与重定向限制测试当时最新已发布的更新资产。先核对签名清单，再对候选源下载完整更新 ZIP、helper，并验证真实断点续传、大小与 SHA-256；按完整传输表现选出最多 3 个源。没有合格源时停止构建。

测速生成本次 `release-<版本数字>/update-mirrors.json` 和构建目录 `src/generated/tclient_update_mirrors.h`，编排器以 `-DTCLIENT_RELEASE_MIRRORS=ON` 编译进客户端。选中源排在原有备用源与官方直链之前；原有 Ed25519/SHA-256 校验继续生效。新包尚未公开时，以已发布包测速，不依赖待发布资产。

报告绑定本次版本、源码提交和生成头文件哈希；构建、stage、打包与发布前校验 24 小时有效期，并核对实际程序中的镜像及阶段 SHA-256。`--resume` 只接受当前阶段顺序的连续完成记录。旧记录缺测速阶段、证据被修改或过期时，需使用新的发布工作根目录重新测速和构建，不能跳过门禁复用旧包。

仅调试探测器可手动运行（正式包仍走编排器）：

```powershell
python -I "$repo/tclient_scripts/probe_update_mirrors.py" `
  --repo $repo --release-version '<本次版本>' --source-commit '<完整提交哈希>' `
  --output '<本次发布目录>/update-mirrors.json' `
  --header "$vsBuild/src/generated/tclient_update_mirrors.h"
if ($LASTEXITCODE -ne 0) { throw 'Mirror probe failed' }
```

`release.yml` 将 Windows 正式资产的生成放在固定的 `windows-2022` runner，使用 Visual Studio 2022。只有来源核对、干净 stage、Authenticode 签名、Ed25519 清单签名和最终资产验证全部成功，Ubuntu 发布任务才会创建 GitHub Release。

本次修复不代表已经签出正式的 `1.7.8.55` 资产。当前没有可用的正式代码签名证书；提供真实证书并补齐构建产物前，发布应当失败。不要用自签证书、修改信任库或放宽验签要求绕过门槛。

## 发布环境配置

在 GitHub 的 `release` environment 中配置以下 secrets。不要把 PFX、密码或 Ed25519 私钥提交到仓库。

| Secret | 内容 |
| --- | --- |
| `TCLIENT_WINDOWS_PFX_BASE64` | 真实代码签名证书及私钥的 PFX，以 Base64 编码 |
| `TCLIENT_WINDOWS_PFX_PASSWORD` | PFX 的非空密码 |
| `TCLIENT_WINDOWS_CERT_THUMBPRINT` | 上述签名证书的 40 位 SHA-1 指纹，可以带空格；用来选择证书，不是文件摘要算法 |
| `TCLIENT_UPDATE_ED25519_PRIVATE_KEY` | 与客户端内置更新公钥匹配的未加密 Ed25519 PEM |

证书必须具有代码签名用途、有效私钥和受 Windows 信任的证书链。工作流使用 `CurrentUser\My`，显式指定指纹，不自动选择其他证书。PFX 应包含需要的中间证书；签名工具不会安装根证书或修改信任库。

可设置 environment variable `TCLIENT_WINDOWS_TIMESTAMP_URL` 为 HTTPS RFC 3161 时间戳地址，默认 `https://timestamp.digicert.com`。文件签名和时间戳摘要均使用 SHA-256，时间戳失败就停止。

工作流先记录证书存储中的现有指纹，最后在 `finally` 中删除临时 PEM、PFX，以及本次新导入到 `CurrentUser\My` 的证书及私钥。已有证书和无关证书不会被删除。清理失败也会使签名步骤失败。

## 构建产物与来源

运行发布前，需要已存在的 `vA.B.C.D` 标签和成功完成的构建 run ID。工作流要求该 run 是本仓库的 `build.yml` 或 `fast-build.yml` 触发的 `push` 构建，其 `head_sha` 与标签最终指向的提交完全一致；该提交中的 `TCLIENT_APP_VERSION` 也必须与标签一致。发布任务不会替用户创建或移动标签。

同一个构建 run 应有以下 artifact 名称：

- `ddnet-windows-latest`
- `ddnet-ubuntu-22.04`
- `ddnet-macOS-latest`
- `ddnet-android`

Windows artifact 必须只包含一个完整 packaged-build ZIP。ZIP 中只能出现一个 `DDNet.exe`，以它所在目录作为包根。包根必须包含完整运行时 DLL 和以下文件：

- `DDNet.exe`
- `island_media_monitor.exe`
- `netease-watcher.exe`
- `wndhok.dll`
- `steam_api.dll`
- `discord_game_sdk.dll`
- `vulkan-1.dll`

`TClientUpdater.exe` 应与客户端一起提供；如果缺失，工作流只允许从已核对的同一提交在独立 CMake 目录原生构建该 updater，再交给签名步骤。监控程序、watcher 和 hook 必须来自完整且经过审查的构建产物，工作流不会从网络补取。

历史托管构建的 Windows ZIP 可能没有音乐辅助程序。它们不能作为正式 `.55` 发布的完整输入；缺失检查会在导入证书之前给出明确错误。需要先准备包含上述文件的完整构建 artifact，本次修改没有扩展其他构建工作流或添加这些工具的外部源码。

## 签名和打包顺序

1. 在新的临时目录解压构建 ZIP，拒绝绝对路径、路径穿越和符号链接。
2. `prepare_release_stage.py` 从包根读取客户端、updater 和所有根目录 DLL，以显式参数读取三个音乐辅助文件。包根也可作为 `--resources`，但该参数只复制允许的非代码资源；同一提交的 `data/` 覆盖游戏资源。新的 stage 和外置 `stage-inventory.json` 记录来源、角色、大小和 SHA-256。
3. `windows_sign_release.ps1 -Mode Sign` 只签署 `DDNet.exe`、`TClientUpdater.exe` 和 `island_media_monitor.exe`。三个文件都必须有受信任且带有效时间戳的 Authenticode 签名。
4. `steam_api.dll`、`discord_game_sdk.dll`、`vulkan-1.dll` 必须保留有效的厂商签名。所有厂商文件，包括 `netease-watcher.exe`、`wndhok.dll` 和其他 DLL，均保留 inventory 记录的原始字节；不得重新签名或替换。
5. `make_release_assets.py` 必须收到 `--stage-inventory`。它在写 ZIP 前调用 Windows 签名验证，按严格规则生成资产，再使用 `--verify --require-authenticode --stage-inventory ... --full-zip ...` 进行只读验证。完整包、更新包及独立程序必须使用相同的已验签文件。

工作流只把以下六个 Windows 文件交给发布任务，并明确列出上传路径：

- `TClient-windows.zip`
- `TClient-update-win64.zip`
- `TClient-update-win64.json`
- `TClient-update-win64.json.sig`
- `TClientUpdater.exe`
- `DDNet.exe`

`windows-release-audit` 是单独的内部 artifact，保存 provenance inventory 和写入所选 checkout 的 `update/` 原始清单对，保留 30 天。它不作为公开 Release 资产。`signed-windows-release` 保存上述六个最终资产，保留 7 天，供同一次工作流的 Ubuntu 发布任务下载。checkout 中的原始清单不会自动提交或推送到分支。

其他平台继续发布原有 Linux、macOS、Android 包。创建 Release 前先确认所有平台资产存在且非空；Windows 更新清单签名必须是 64 字节原始 Ed25519 签名。

## 本地准备与验证

以下示例需要 Windows、Visual Studio 2022 C++ 工具链、Python 的 `cryptography` 包、Windows SDK SignTool、完整 Release 构建目录，以及已安装的真实代码签名证书。示例路径是占位路径；stage 和报告必须尚不存在，父目录需已存在。

使用 VS2022 的独立构建目录，避免复用其他生成器的 CMake cache。构建客户端和 updater 的示例命令如下；这一步不会生成三个音乐辅助文件，仍需单独提供经过审查的原始文件和完整依赖 DLL。

```powershell
$repo = 'D:/TaterClient-src'
$vsBuild = 'D:/TaterClient-src/build-vs2022-v17855'
cmake -S $repo -B $vsBuild -G 'Visual Studio 17 2022' -A x64 `
  -DDISCORD=ON -DVULKAN=ON -DTOOLS=OFF -DDOWNLOAD_GTEST=OFF -DPREFER_BUNDLED_LIBS=ON
if ($LASTEXITCODE -ne 0) { throw 'VS2022 configuration failed' }
cmake --build $vsBuild --config Release --target game-client TClientUpdater --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'VS2022 build failed' }
```

`--build-dir` 可以直接使用这个新构建的 `Release` 目录；此时将 `--monitor`、`--watcher` 和 `--hook-dll` 分别指向经过审查的文件，不要把它们从历史目录整树混入。下面演示输入已整理为完整 packaged-build 根目录的情况。

```powershell
$repo = 'D:/TaterClient-src'
$build = 'D:/release-input/complete-build'
$stage = 'D:/release-work/fresh-stage'
$report = 'D:/release-work/stage-inventory.json'

python -I "$repo/tclient_scripts/prepare_release_stage.py" `
  --build-dir $build --output $stage --report $report --resources $build `
  --data-dir "$repo/data" `
  --monitor "$build/island_media_monitor.exe" `
  --watcher "$build/netease-watcher.exe" --hook-dll "$build/wndhok.dll"
if ($LASTEXITCODE -ne 0) { throw 'Stage preparation failed' }

powershell.exe -NoProfile -NonInteractive -File "$repo/tclient_scripts/windows_sign_release.ps1" `
  -Directory $stage -Mode Sign -CertificateThumbprint '<真实证书的40位指纹>' `
  -CertificateStoreLocation CurrentUser -TimestampUrl 'https://timestamp.digicert.com'
if ($LASTEXITCODE -ne 0) { throw 'Authenticode signing failed' }

powershell.exe -NoProfile -NonInteractive -File "$repo/tclient_scripts/windows_sign_release.ps1" `
  -Directory $stage -Mode Verify
if ($LASTEXITCODE -ne 0) { throw 'Authenticode verification failed' }
```

完成真实签名后，使用匹配的 Ed25519 私钥调用严格打包器，再执行严格的只读验收。版本必须与实际已核对的源码和标签一致。

```powershell
python -I "$repo/tclient_scripts/make_release_assets.py" `
  --source $stage --stage-inventory $report --version '1.7.8.55' `
  --output 'D:/release-work/assets' --private-key 'D:/release-secrets/update-ed25519.pem' `
  --full-zip 'D:/release-work/assets/TClient-windows.zip' `
  --raw-manifest-dir "$repo/update" --notes-file 'D:/release-work/notes.txt' --quiet
if ($LASTEXITCODE -ne 0) { throw 'Strict packaging failed' }

python -I "$repo/tclient_scripts/make_release_assets.py" `
  --verify --require-authenticode --stage-inventory $report --version '1.7.8.55' `
  --output 'D:/release-work/assets' --full-zip 'D:/release-work/assets/TClient-windows.zip' `
  --raw-manifest-dir "$repo/update"
if ($LASTEXITCODE -ne 0) { throw 'Final asset verification failed' }
```

单独的 `--verify` 继续用于旧版 Ed25519 清单验收，不能作为新版 Windows 正式发布签名验收的替代。没有真实代码签名证书时，可以准备 stage 和 inventory，并确认签名验证拒绝未签名文件；不要将它们标为正式资产或上传。
