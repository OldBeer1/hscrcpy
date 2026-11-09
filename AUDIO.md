# 音频转发

当前实现包含 Windows 接收播放端和独立的鸿蒙手机端采集应用。Windows 端已完成编译与本地回环测试；手机端已完成 SDK 编译、调试签名和当前鸿蒙 7（API 26）手机的安装验证。真机测试中 Windows 显示 Playing phone audio，用户确认电脑播放声音正常；关闭音频和重新开启后恢复播放的交互检查通过。

## 使用

1. 使用 DevEco Studio 构建并签名下方的手机端工程。签名后的应用需要允许安装到目标手机。
2. 将签名 HAP 放到 Windows 程序旁的 server/audio-capture.hap。也可以事先通过 HDC 安装该应用。
3. 连接手机，正常启动投屏，勾选工具栏第三行的 Audio。
4. 在手机上确认系统音频采集授权。Windows 播放设备将收到手机内部播放的声音。
5. 取消 Audio、停止投屏或退出程序会结束本会话，并移除本会话创建的 HDC 反向端口映射。

首次使用默认开启 Audio；开关保存到程序旁 hscrcpy.ini 的 [Audio] Enabled。应用缺失、拒绝授权、设备断开或播放失败均不关闭视频投屏。缺失应用时显示 Audio app needed；按本页安装后关闭再打开 Audio 重试。

命令行 --audio 强制开启，--no-audio 关闭；未传参数时使用保存的设置。两个参数同时出现时，以最后一个为准。

需要更换电脑或手机时，使用包含独立签名工具的迁移包，完整流程见 [MIGRATION.md](MIGRATION.md)。

## 本机日常连接与断开

本机已完成手机端签名安装。日常使用现有的 build/hscrcpy.exe 和手机上已安装的音频应用即可。先退出 DevEco Studio，再开始日常投屏，避免 IDE 退出时影响正在使用的共享调试连接；不需要 H: 映射。

1. 电脑开机后，解锁手机，用能传数据的 USB 线连接电脑，保持手机 USB 调试开启。
2. 手机如出现 USB 调试或电脑连接授权提示，由本人确认允许。
3. 双击 build/hscrcpy.exe，保留同目录内的 DLL、hdc.exe、server、res 等配套文件。
4. 点击设备框旁的刷新按钮，选择手机，再点三角形播放按钮（启动投屏），等待出现画面。
5. 勾选 Audio，在手机上确认系统采集授权。播放手机音乐或视频，电脑通过 Windows 当前默认扬声器/耳机输出声音。
6. 只想停止声音时取消 Audio；结束本次使用时点方形停止按钮，或正常关闭 hscrcpy 窗口。
7. 等手机的系统采集提示结束后，再拔 USB 线。手机端应用保留，供下次继续使用。

若设备框显示 0 device(s)，先解锁手机、检查 USB 连接并点击刷新。若仍显示 Offline，可在 build/ 目录打开 PowerShell，仅在故障时执行以下命令重建电脑端调试连接：

    .\hdc.exe start -r
    .\hdc.exe list targets -v

看到 Connected 后，在 hscrcpy 中刷新设备并重新启动投屏。如手机再次出现系统授权提示，由本人确认。本机不需要每次重新编译、签名或安装应用；更换手机或签名失效时需要单独处理。

## 手机端构建

工程位于 audio-companion/，包名为 com.hscrcpy.audiocapture，Ability 为 EntryAbility，采用 ArkTS + C++17，不使用系统签名或免授权权限。

1. 首次检出时，将 audio-companion/build-profile.example.json5 复制为同目录的 build-profile.json5；已有本地配置时保留原文件。使用 DevEco Studio 打开 audio-companion/，准备 HarmonyOS 公共 SDK、Native 工具链和与工程匹配的 Hvigor 依赖。
2. 当前配置最低 API 12，目标 SDK 为 HarmonyOS 6.0.0 (API 20)，Native ABI 为 arm64-v8a。本机已使用 Command Line Tools 26.0.0.865 内置的 SDK 26.0.0.105 编译通过，最低与目标版本保持原配置。命令行构建步骤见 audio-companion/README.md。
3. 在 IDE 中为自己的设备配置自动签名，确保默认产品的 signingConfig 指向对应签名配置，再构建 entry 模块的 HAP。本地 build-profile.json5 已排除出 Git，可公开的模板是 build-profile.example.json5；不要提交证书、私钥、Profile 或签名密码。
4. 将签名产物复制到 server/audio-capture.hap 和 build/server/audio-capture.hap。未签名 HAP 不能用于消费者版手机。

也可以先手动安装：

    hdc -t <设备标识> install -r <签名后的HAP路径>

本机已生成签名产物 audio-companion/entry/build/default/outputs/default/entry-default-signed.hap（1,494,895 字节），HDC 安装成功，设备包管理器已确认包名和版本 1.0.0。已将同一签名包放到 server/audio-capture.hap 和 build/server/audio-capture.hap。

本机直接运行 build/hscrcpy.exe，选择手机并启动投屏，勾选 Audio，在手机上确认系统采集授权。build/ 中已配齐项目 SDK 的 hdc.exe 和 libusb_shared.dll；从不含额外工具路径的进程环境启动 HDC，能正常识别手机，不需要全局 PATH 设置。保留程序旁的配套文件。

目前只验证了当前授权设备的基本音频播放、开关和恢复。长时间运行、锁屏、其他手机/应用、受保护内容以及拔线重连仍需分别验证。IDE 对带空格的路径有限制，开发时曾通过临时 H: 入口打开工程；本次已退出 IDE 并撤销该映射，原项目文件完整保留在 C:\Users\Old Beer\Projects\hscrcpy。日常运行不依赖 IDE 或 H:。

## 采集和播放行为

- 只采集系统内部播放音频，不采集麦克风，不传输视频数据。
- 使用公开 OH_AVScreenCapture 接口，保留系统采集授权弹窗和状态提示。
- 允许后台任务后才移动到后台；后台运行申请失败时保持可见页面，状态会提示保持前台。
- 取消授权、系统停止、来电等终止状态会释放采集资源。隐私场景暂停发送音频，保留心跳。
- 不改变手机音量；可能同时从手机和电脑发声。某些受保护内容、通话或禁止采集的应用不能据此保证可传输。
- 使用 Windows 默认播放设备，PCM 48 kHz、双声道、16 bit。运行中更换输出设备时可关闭再打开 Audio。
- 音视频独立传输，当前没有额外的音画延迟校准；实际延迟和锁屏行为需要真机验证。
- 同一手机端应用只有一个采集会话；不同手机使用独立监听端口和会话令牌。

## 连接及资源管理

Windows 端仅监听 127.0.0.1 的系统分配端口，通过 HDC rport 将手机的本地 TCP 端口反向转发到电脑。启动参数携带随机 128 bit 会话令牌，接收端校验令牌及音频格式，防止旧会话串流。

首包为 52 字节：HSA1、版本 1、采样率、声道、位深（整数均为 u32 小端），后接 32 个十六进制 ASCII 令牌字符。每个音频包为 u32 小端长度 + PCM；长度 0 为心跳。非零长度必须按 4 字节采样帧对齐，最大 19200 字节。

采集回调只复制数据，网络发送在独立线程执行。两端音频队列限制为 80 ms，避免积累长时间旧音频；这不是对实际端到端延迟的保证。断开时只清理当前音频会话，不全局清理 HDC 映射，不卸载手机应用。

## Windows 验证

已有本地构建目录可执行：

    .\.tools\cmake-4.4.5-windows-x86_64\bin\cmake.exe -S . -B build -DHSCRCPY_BUILD_TESTS=ON
    .\.tools\cmake-4.4.5-windows-x86_64\bin\cmake.exe --build build --parallel 4
    .\.tools\cmake-4.4.5-windows-x86_64\bin\ctest.exe --test-dir build --output-on-failure

编译时仅将 .tools/w64devkit/bin 加入当前 PowerShell 进程的 PATH。该设置不写入系统环境变量。新机器首次构建仍需按 README 准备依赖。

测试覆盖协议格式和令牌、TCP 分包、心跳、实际 Windows 静音 PCM 播放、非法及截断包、等待连接时取消、多监听实例，以及模拟 HDC 的关闭、缺失应用、启动、重启和精确映射清理。测试用 HDC 只放在 build/audio-tests/，不连接真实设备。

## 参考

- 华为公开音频采集说明：https://developer.huawei.com/consumer/en/doc/best-practices/bpta-audio-record-base-on-avscreencapture
- OpenHarmony 公开接口定义：https://github.com/openharmony/interface_sdk_c/blob/master/multimedia/player_framework/native_avscreen_capture.h
- Windows WaveOut：https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutwrite

新音频模块和手机端代码按本仓库 Apache-2.0 许可提供，未引入 jmScrcpy 的 BSL 源码。
