# 构建、签名与发布

项目提供 Windows x64 构建、测试、打包、SHA-256 校验和 GitHub 构建来源证明流程。当前 CI 生成的 EXE **未做 Windows Authenticode 签名**；来源证明与校验和不会改变这个状态。工作流只保存 Actions 构建产物，不会创建仓库、推送提交或发布 GitHub Release。

## 在 GitHub 构建

本项目仓库为 [bcosn/hdd-noise](https://github.com/bcosn/hdd-noise)。`.github/workflows/build.yml` 会在 push、pull request 和手动运行时执行以下步骤：

1. 使用 MSVC、CMake 构建 Release x64 程序，静态链接 MSVC 运行库。
2. 运行 `SynthTests`，校验程序合成的声音。CI 不运行需要真实音频设备的集成测试，也不播放声音。
3. 从 `VERSION` 读取版本，通过 `scripts/package.ps1` 生成 ZIP 与 `SHA256SUMS.txt`。
4. 在该次 Actions 运行的 Artifacts 区域保存 `hdd-noise-windows-x64-unsigned-…`，保留 30 天。下载后，外层 Actions 压缩包中包含实际分发 ZIP 和校验文件。

`VERSION` 使用 `0.1.0` 或 `0.1.0-beta.1` 这类格式。所有第三方 Actions 均来自 GitHub 官方 `actions` 组织，并固定到完整提交 SHA；升级时应核对官方发行记录与对应提交，而不是只替换版本注释。

打包使用明确的文件清单：程序、README、声音设计说明、签名与发布说明，以及记录版本和签名状态的 `release-info.json`。图标、清单和版本资源已编译进 EXE。用户自己的 `hdd-noise.ini`、录音参考、音频分析文件、构建依赖、证书与私钥均不放入分发包。

## GitHub 构建来源证明

公开仓库的默认分支收到 push 后，会为该次已测试构建生成的 ZIP 自动生成来源证明。私有仓库需先确认自己的 GitHub 套餐支持此功能，再在仓库 Actions variables 中设置 `ENABLE_ARTIFACT_ATTESTATIONS` 为 `true`。也可在默认分支手动运行工作流并勾选 `attest`。手动运行的默认值为关闭。

pull request、标签及非默认分支构建只生成普通构建产物，不生成证明。普通构建只有仓库内容读取权限；独立的证明任务才获得 `id-token: write` 和 `attestations: write`，且只下载同次运行的构建产物，不检出或执行仓库脚本。整个工作流不读取签名证书秘密，也不向 GitHub Releases 上传文件。原理与可用范围见 [GitHub 官方构建来源证明文档](https://docs.github.com/en/actions/security-for-github-actions/using-artifact-attestations/using-artifact-attestations-to-establish-provenance-for-builds)。

来源证明用于核验 ZIP 来自哪个仓库、提交和构建流程；它不是 EXE 的 Windows 发布者签名，也不会让 Windows 自动信任该程序。下载实际分发 ZIP 后，可使用 GitHub CLI 检验，使用本项目仓库名称：

```powershell
gh attestation verify .\hdd-noise-0.1.0-windows-x64.zip --repo bcosn/hdd-noise
```

请在 Actions 中查看证明任务成功后的摘要及证明记录。未运行或失败的证明任务不能当作已完成证明。

## 本地构建与打包

安装带 C++ 桌面开发组件的 Visual Studio 或 Build Tools，以及 CMake。在项目根目录运行：

```powershell
cmake -S . -B build/ci -A x64 -DBUILD_TESTING=ON -DHDD_AUDIO_INTEGRATION_TESTS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_EXE_LINKER_FLAGS=/Brepro
cmake --build build/ci --config Release --parallel
ctest --test-dir build/ci -C Release --output-on-failure --no-tests=error
$releaseVersion = (Get-Content -LiteralPath VERSION -Raw).Trim()
./scripts/package.ps1 -Executable 'build/ci/Release/hdd-noise.exe' -Destination 'out/release' -Version $releaseVersion
```

产物位于 `out/release/`。`/Brepro` 避免链接器写入当前构建时间；这并不承诺不同编译器或 SDK 版本之间的 EXE 逐字节相同。发布前应实测程序图标、托盘显示、音量设置保存、磁盘选择、停止与退出。

## 校验下载文件

在存放 ZIP 与 `SHA256SUMS.txt` 的文件夹中运行以下命令，并将版本号换成实际下载的版本：

```powershell
$packageName = 'hdd-noise-0.1.0-windows-x64.zip'
$checksumPattern = '^([0-9A-Fa-f]{64}) [ *]' + [regex]::Escape($packageName) + '$'
$checksumLine = @(Get-Content -LiteralPath .\SHA256SUMS.txt | Where-Object { $_ -match $checksumPattern })
if ($checksumLine.Count -ne 1) { throw '校验文件中没有唯一对应的 ZIP 记录。' }
$expectedHash = $checksumLine[0].Substring(0,64)
$actualHash = (Get-FileHash -LiteralPath $packageName -Algorithm SHA256).Hash
if ($actualHash -ine $expectedHash) { throw 'ZIP 校验失败。' }
Write-Output 'ZIP 的 SHA-256 校验通过。'
```

校验和可发现文件损坏或与提供的哈希不一致；若 ZIP 与校验文件一起被替换，单靠校验和不能认证发布者。需要时再核验来源证明或 Authenticode 签名。

## 为 Windows 发行版签名

按 [SIGNING.md](SIGNING.md) 使用自己已有的代码签名证书或受信任的签名服务，对最终 EXE 做 Authenticode 签名并验证。开发自签名证书只适用于测试，不能冒充受信任的公开发行证书。不要把 `.pfx`、私钥、密码、硬件令牌凭据或个人证书配置提交到仓库或加入分发包。

正确顺序是：完成资源与版本修改 → 构建与测试 → 签名并验证 EXE → 打包 → 生成 ZIP 校验和与对应来源证明。签名会改变文件内容，签名后必须重新打包；已有的 ZIP 哈希与来源证明不自动适用于新 ZIP。签名也不保证消除 Microsoft Defender SmartScreen 提示，相关机制见 [Microsoft 官方 SmartScreen 说明](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)。

当前 CI 未接入正式证书，保留 `unsigned` 产物标识。未来接入签名服务时，应单独设计仅允许受信任发行提交执行的签名任务，不向 fork PR 开放证书或签名权限。
