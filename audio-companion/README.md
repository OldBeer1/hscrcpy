# hscrcpy 鸿蒙音频采集应用

这是 hscrcpy 的独立手机端工程。构建签名、安装、协议、后台限制和当前验证范围参见仓库根目录 AUDIO.md。

- 包名：com.hscrcpy.audiocapture
- Ability：EntryAbility
- 采集：公开 AVScreenCapture 内录接口，PCM 48 kHz / 2 channels / 16 bit
- 权限：INTERNET、KEEP_BACKGROUND_RUNNING；不申请麦克风或免授权系统权限
- 运行：由 Windows 端使用 HDC 启动，手机用户确认系统授权
- 构建：DevEco Studio + HarmonyOS 公共 SDK + Native 工具链
- 签名：自行配置消费者版设备信任的调试/发布签名；仓库不提供签名材料

2026-11-05 已使用 Command Line Tools 26.0.0.865、Hvigor 6.26.9 和 HarmonyOS SDK 26.0.0.105 完成 ArkTS、Native ARM64 编译及调试签名，生成 entry/build/default/outputs/default/entry-default-signed.hap，并安装到已连接的鸿蒙 7（API 26）手机。Windows 收到内部音频，用户确认电脑播放声音正常；音频关闭及重新开启后恢复播放的交互检查通过。

本机命令行构建（在本目录中运行；Java 路径按本机 DevEco Studio 安装位置调整）：

```powershell
if (-not (Test-Path '.\build-profile.json5')) {
  Copy-Item '.\build-profile.example.json5' '.\build-profile.json5'
}
$env:JAVA_HOME = 'C:\Software\DevEco Studio\jbr'
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& '..\.tools\command-line-tools\bin\ohpm.bat' install --all --cache '..\.tools\ohpm-cache'
& '..\.tools\command-line-tools\bin\hvigorw.bat' assembleHap --mode module -p module=entry@default -p product=default -p buildMode=debug --no-daemon
```

上述环境变量只影响当前 PowerShell 进程；不需要写入系统 PATH。首次检出时从不含签名材料的 build-profile.example.json5 创建本地 build-profile.json5。后者可能保存签名密码，已排除出 Git；保留已有本地配置，不要覆盖它。保持 signingConfigs 为空时生成未签名 HAP；配置调试签名时还需将产品的 signingConfig 关联到对应配置（本机为 default）。
