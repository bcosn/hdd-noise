# EXE 签名与构建证明

仓库提供正式代码签名的接入流程，不包含签名证书、私钥或密码。准备这些脚本不会给 EXE 添加签名，也不会创建证书或修改系统信任设置。没有正式证书时，可以发布明确标注为未签名的构建，并提供 SHA-256 校验值和 GitHub 构建证明。

## 三种信息分别证明什么

| 信息 | 作用 |
| --- | --- |
| Windows Authenticode 签名 | Windows 检查 EXE 的签名及发布者证书。发布脚本要求结果为 `Valid`，并有经过验证的时间戳。 |
| GitHub 构建证明 | 将产物的摘要关联到仓库、提交和构建工作流；它不会在 EXE 上增加 Windows 发布者签名。 |
| SHA-256 校验值 | 检查下载内容是否与发布者提供的文件一致；校验值本身不提供发布者身份。 |

不要把 GitHub 构建证明、哈希校验或自签名证书描述为 Windows 认可的正式发布者签名。正式签名也不能保证绕过 SmartScreen 或其他安全软件的提示；发布者身份验证和下载信誉是不同的检查。

## 正式签名前的条件

- Windows 以及 Windows SDK 中的 SignTool。
- 一张正式代码签名证书，已由证书提供方配置到当前用户的 `Cert:\CurrentUser\My`，并且可访问对应私钥。硬件密钥或供应商密钥提供程序应先按供应商说明配置。
- 证书仍在有效期内，包含 Code Signing EKU，允许数字签名，并能通过 Windows 的发布者信任检查。
- 可连接的 HTTPS RFC 3161 时间戳服务。默认使用 `https://timestamp.digicert.com`；也可传入证书提供方支持的 HTTPS 地址。

脚本不会自动导入 PFX、接受明文私钥密码、导出私钥或安装根证书。云签名服务若不向当前用户证书存储区提供可用的签名密钥，需要另接供应商的签名工具；不要把云签名凭据拼进命令行。

先完成构建及资源嵌入，再签名。签名后重新编译或修改 EXE，需要重新签名；最终压缩包及 `SHA256SUMS` 也应在签名完成后生成。

## 本机正式签名

先用以下只读命令查看已有代码签名证书。证书的 Subject 和 Thumbprint 是公开身份信息，不是私钥：

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Select-Object Subject, Thumbprint, NotAfter, HasPrivateKey
```

确认所选证书后执行：

```powershell
$thumbprint = Read-Host '输入正式代码签名证书的 Thumbprint'
.\scripts\sign.ps1 -Path .\out\hdd-noise.exe `
    -CertificateThumbprint $thumbprint `
    -TimestampUrl 'https://timestamp.digicert.com'
```

脚本在 PATH 和已安装 Windows SDK 中查找 SignTool，也支持显式指定 `-SignToolPath`。`-WhatIf` 可以预览签名目标，但仍会检查输入文件、证书及工具是否存在。默认拒绝覆盖已有签名；优先使用新构建，确实需要替换时显式添加 `-ReplaceExistingSignature`。

签名使用 SHA-256 文件摘要与 SHA-256 RFC 3161 时间戳。脚本先在临时副本上操作，再执行 `signtool verify /pa /all /tw`，并检查 Windows `Get-AuthenticodeSignature` 返回 `Valid`、签名证书与指定指纹一致，以及存在时间戳证书。所有检查通过后才替换原 EXE，并输出最终签名状态和 SHA-256。

证书缺失、过期、私钥不可用、时间戳失败、签名不受信任、验证警告或文件被并行构建修改，都会使操作失败；验证失败的临时副本不会替换原文件。没有自签名放行选项，也不会通过修改信任设置让验证结果变为 `Valid`。网络或证书服务暂时不可用时，应排查后重试。

## GitHub 发布时

构建证明工作流不需要代码签名私钥，但它不会自动完成 Authenticode 签名。没有正式签名凭据的构建应保留真实的 `NotSigned` 状态，并连同校验文件一起发布。

正式签名应在受控的签名环境中完成。不要把 PFX、私钥、硬件令牌 PIN 或签名服务凭据提交到仓库、上传为普通构建产物，或写入公开日志。若签名由独立步骤完成，构建证明和最终校验值必须对应签名后的最终文件；不能拿签名前文件的摘要替代。

下载后，可在 Windows 检查：

```powershell
Get-AuthenticodeSignature -LiteralPath .\hdd-noise.exe |
    Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
Get-FileHash -LiteralPath .\hdd-noise.exe -Algorithm SHA256
```

比较发布页提供的摘要及发布者身份。构建证明的验证命令和适用产物以仓库的 GitHub 发布说明为准。

## Microsoft 官方资料

- [SignTool：签名、摘要算法、RFC 3161 时间戳及验证选项](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)
- [SignTool 的时间戳参数顺序及 Authenticode 验证](https://learn.microsoft.com/en-us/dotnet/framework/tools/signtool-exe)
- [Get-AuthenticodeSignature：检查 Windows 签名状态](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.security/get-authenticodesignature)
- [时间戳服务的 HTTPS 示例](https://learn.microsoft.com/en-us/windows/win32/seccrypto/adding-time-stamps-to-previously-signed-files)
- [Microsoft Defender SmartScreen 与应用信誉](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
