# 本地验证记录

验证日期：2026-11-04。项目版本：2.0.2。

## 来源

- 直接上游：https://gitcode.com/pengxiaochuan/hscrcpy
- 导入基线：`ef89af6eaa90acece6aed4a36b2df73af5024885`
- 保留原始 Git 历史、Apache-2.0 许可证和第三方许可证。

## 本次最小修改

- `.gitattributes`：将随源码保存的 nlohmann JSON 头文件标为普通文本，取消与实际存储不一致的 LFS 声明。
- `CMakeLists.txt`：构建前检测 IPO/LTO 能力。支持时启用，不支持时正常构建，不改变投屏功能代码。
- `.gitignore`：排除项目内工具链及本地验证日志。

## 验证环境

- Windows 11 x64。
- 编译器：w64devkit 2.11.0，GCC 16.2.0。
- CMake 4.4.5。
- nghttp2 1.71.0，静态库构建。
- FFmpeg：使用上游仓库自带的共享库。

工具仅保存在 `.tools/`，未添加到系统 PATH。工具下载包已按官方 GitHub release 的 SHA-256 校验；工具、构建产物及详细日志不纳入版本控制。

## 已执行检查

- Git 对象完整性检查。
- PowerShell 构建脚本语法检查。
- 常见敏感文件名及凭据特征扫描：未发现匹配项；此项不构成全面安全审计。
- CMake Release 配置与完整编译：通过。
- 程序 `--help` 启动测试：退出码 0，输出版本和帮助信息。
- 无效命令行参数测试：退出码 1，符合预期。
- Windows 运行库加载：上述启动测试通过；本地构建目录已配齐所需 FFmpeg DLL、server 和 res 资源。

## 尚未验证

当前机器未配置 HDC，因此未连接手机，未进行鸿蒙 7 真机投屏、反控、录像和长时间稳定性测试。编译通过不代表所有设备兼容性已验证，此段记录的是导入基线的验证范围，新增音频功能的验证见下文。

## 本地目录

- `.tools/`：便携工具和依赖，仅供本项目使用。
- `.validation/`：编译及启动检查日志。
- `build/hscrcpy.exe`：本机 Release 构建产物。

本次便携构建使用 `MinGW Makefiles` 生成器，nghttp2 安装到 `.tools/nghttp2-local/ucrt64`，并通过 CMake 参数 `NGHTTP2_ROOT` 指定 `.tools/nghttp2-local`。原 `build.ps1` 仍面向 MSYS2，不会自动识别这套便携工具。

# 音频功能本地验证（2026-11-05）

- 新增 Windows 音频接收播放、独立开关及命令行配置，以及鸿蒙音频采集应用源码。
- Windows Release 构建通过。
- CTest 音频测试通过，覆盖格式/令牌、分包、心跳、实际 Windows 静音播放、非法及截断数据、多监听、取消，以及假 HDC 会话启停和映射清理。
- 手机端已完成 ArkTS、Native ARM64 编译、调试签名和当前鸿蒙 7 手机的安装；真机音频播放由用户确认正常，开关及恢复检查通过。
- 当前手机的基本音频转发已验证；这不代表所有鸿蒙 7 设备和应用均兼容，其他设备仍需按 AUDIO.md 配置签名、安装和实测。

- 音频参数命令行启动检查、12 个手机端 JSON 工程配置解析、共用头文件路径和 SVG 资源检查通过。

## 手机端首次 SDK 构建（2026-11-05）

- 工具：Command Line Tools 26.0.0.865、Hvigor 6.26.9、OHPM 26.0.0.630、HarmonyOS SDK 26.0.0.105（API 26）。
- Java：复用已安装 DevEco Studio 的 JBR 25.0.2，仅为当前构建进程设置 JAVA_HOME 和 PATH。
- 执行 ohpm install --all（缓存位于 .tools/ohpm-cache），随后执行 hvigorw assembleHap --mode module -p module=entry@default -p product=default -p buildMode=debug --no-daemon。
- 初次编译的 Native 和 ArkTS 均通过；打包因 java 未在进程 PATH 中失败，补齐进程 PATH 后构建成功，退出码 0。
- 产物：audio-companion/entry/build/default/outputs/default/entry-default-unsigned.hap，1,427,405 字节。
- SHA-256：39317763298FCC9C21EFBA051FBE7ABF3105F30B0E3157EDFDCFE4B92D6D6225。
- 包内容检查：包含 ets/modules.abc、resources.index、arm64-v8a/libaudio_capture.so 和 libc++_shared.so；包名 com.hscrcpy.audiocapture，版本 1.0.0，最低 API 12、目标 API 20、实际编译 SDK 26.0.0.105；权限仅 INTERNET 和 KEEP_BACKGROUND_RUNNING。
- 最终构建日志：.validation/audio-companion-build.log。未配置签名的提示符合本阶段预期。
- 非阻断警告：app_name 在 AppScope 与 entry 重复定义；SDK 生成的 --gcc-toolchain 参数未使用；API 12 平板不支持 moveAbilityToBackground，相关后台行为尚待设备验证。
- 本阶段未连接手机、未签名、未安装、未提交或推送。SDK 构建通过不代表鸿蒙 7 真机音频转发已通过。

## 手机签名安装及真机音频（2026-11-05）

- 手机报告 OpenHarmony-7.0.0.111、API 26。用户完成 IDE 工程信任、华为账号登录及授权。
- IDE 自动生成本机调试签名材料，将 default 产品关联到 default 签名配置后，Hvigor assembleHap 签名构建成功，退出码 0。
- 签名产物：audio-companion/entry/build/default/outputs/default/entry-default-signed.hap，1,494,895 字节；SHA-256：8DCB3BFE0411703660635B1656C0B10F1D457649AA4C20CDB9B46D4D58314695。
- HDC install -r 返回 install bundle successfully；bm dump 确认 com.hscrcpy.audiocapture、版本 1.0.0 / versionCode 1000000 和 EntryAbility。
- Windows 真机投屏收到首帧，采用 gRPC，显示尺寸 1260x2844。音频接收端显示 Playing phone audio；用户通过手机播放内容并确认“能听到，声音正常”。
- 音频开关的交互检查：关闭后界面显示 Audio off，原 57693 音频端口的映射和 Windows 监听/连接被清理，10038 视频和 10039 控制映射仍保留；重新开启后使用 55608 新端口建立连接，界面恢复 Playing phone audio，投屏持续接收帧。
- 将官方 SDK 的 hdc.exe、libusb_shared.dll 配到 build/；在仅包含 Windows 系统目录的进程 PATH 下，从 build/ 执行 hdc -v 和 hdc list targets 成功。签名 HAP 已复制到 server/audio-capture.hap 和 build/server/audio-capture.hap；均为本地文件，不纳入 Git。
- 自动签名产生的本地 audio-companion/build-profile.json5 已加入 .gitignore；新增不含签名材料的 build-profile.example.json5，供新检出工程创建本地配置。IDE 使用临时 H: 入口访问原目录，没有移动源码。
- 相关记录保存在 .validation/audio-companion-signed-build.log、audio-companion-install.log、audio-companion-installed-bundle.json、audio-off-mappings.log，以及本地 build/hscrcpy.log。
- 本次未执行提交或推送。尚未量化音画延迟，也未做长时稳定性、其他设备/应用、锁屏及拔线重连验证。

## 移除临时 H: 映射及运行确认（2026-11-05）

- 使用 IDE 正常退出流程关闭 DevEco Studio，未强制结束进程或删除任何项目文件。
- 核实 H: 映射指向原项目 C:\Users\Old Beer\Projects\hscrcpy 后，执行 subst H: /D，仅撤销盘符映射。
- 原项目和 build/hscrcpy.exe 均存在；本地调试签名的证书、Profile、密钥库路径仍有效。
- 用户报告关闭 IDE 后连接曾中断，检查时电脑端 HDC 进程已变化，随后手机显示 Connected。未定位 IDE 内部触发的具体命令，不将其归因为已确认的 SDK 缺陷。
- 在 DevEco Studio 已退出、H: 已撤销的状态下，投屏界面有实时帧率，音频显示 Playing phone audio；手机连接和视频、控制、音频映射正常。
- AUDIO.md 补充本机从开机连接到停止及拔线的日常步骤，以及 Offline 时的手动恢复办法。


## 完整迁移包验证（2026-11-08）

- 新增 deployment/Assistant.ps1、phone-tools.mjs、SensitiveSigner.java、Build-Kit.ps1、Export-Kit.ps1，以及 MIGRATION.md 和迁移测试。
- 内置固定版本官方 DevEco CLI、Node、Java、HDC 和签名组件；公开包仅包含未签名手机 HAP 与空签名模板，不包含本人的调试 HAP、p12、Profile、密码或账号缓存。
- 私有标准输入桥接已用实际签名通过验证：HAP 签名阶段的解密密码不放入操作系统进程参数。官方 CLI 本身仍使用其上游实现，未擅自修改。
- 在只保留 Windows 系统目录的进程 PATH 下，Windows PowerShell 5.1 检测手机、官方签名流程、原签名安装和“开始使用.cmd”自动传递设备参数均通过。
- 迁移目录 artifacts/hscrcpy-2.0.2-migration-20261108 的程序已实际启动；用户确认投屏画面与声音均正常。
- 迁移测试 7 项全部通过。完整公开文件清单校验通过。
- 实测修复：CMD 的 -Action 参数换行；对全局 PowerShell 搜索路径的依赖；Windows PowerShell 5.1 参数默认值中脚本目录为空；读取 UTF-8 清单时中文文件名乱码；自动启动未传设备标识。
- 新增签名续期入口；签名不一致时不会自动卸载已有应用。恢复连接会等待手机重新 Connected，并明确失败状态。
- ZIP 从清单白名单导出并生成 SHA-256；运行时生成的个人设置、日志和签名目录不进入 ZIP。
- 当前只对现有电脑与手机做了物理验证；真正的新电脑、其他手机型号及其 USB 驱动尚无实测条件。未提交或推送本轮修改。

## 鼠标点击和滚轮修复（2026-11-08）

- 用户确认：手机手指点同一视频可以暂停，电脑鼠标不能；图文滚轮表现为不动或卡顿。
- 电脑端修复：按下、移动和松开统一由同一工作线程顺序发送；成功的 socket 松开不再重复发 shell 松开；单次手势保持同一输入通道。拖动保留最终坐标。
- 处理 WM_LBUTTONDBLCLK 的第二次按下，并在窗口失去鼠标捕获时释放触控。滚轮先把 Windows 屏幕坐标转换成画布坐标。
- 滚轮改为同一任务内完成的滑动，合并待处理滚轮增量，保留高精度滚轮的小增量并限制屏幕边界；上滑/左滑插值改用有符号差值，避免无符号下溢。
- 新增 common/input_gesture.h、tests/input_tests.cpp，扩展 fake_hdc 与 CMake 测试目标。9 个回归场景覆盖顺序、快速连点、连续拖动隔离、完整滚轮任务、高精度增量、反向抵消、坐标边界、无重复 shell 松开及手势通道一致性。测试使用本地回环和模拟 HDC，不操作真实手机。
- Release 构建通过；ctest --test-dir build --output-on-failure：音频测试与输入测试均通过，0 失败。
- 已在现有迁移目录替换电脑端程序并重新连接现有手机，UI 显示投屏帧率与 Playing phone audio，控制 socket 已连接；未重装手机应用。用户复测确认视频暂停恢复正常，图文滚轮有所改善但仍卡顿；后续平滑优化见下文。
- 未提交或推送本次改动。

## 连续滚轮平滑处理（2026-11-09）

- 第一版用户复测：视频单击暂停恢复正常，图文滚轮明显改善但仍有卡顿，因此继续优化滚轮。
- 修改 src/input_injector.cpp/.h：滚轮在正在执行的滑动中继续累计增量，短暂空闲后释放；接近屏幕边缘才重新落指。点击或按键入队后封闭当前滚轮批次，保持输入顺序。
- 采用约 8 毫秒目标间隔的私有高精度等待计时器与自适应追赶步长；不修改系统计时器分辨率、全局 PATH 或环境配置。计时器创建失败时退回普通线程等待。API 依据：[CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)。
- tests/input_tests.cpp 新增执行中合并滚轮、滚轮途中点击隔离及执行中反向抵消；输入回归场景由 9 项增加到 12 项。Release 构建与 CTest 输入/音频两个测试组均通过。
- 同机本地回环对比：12 次滚轮增量、每次间隔 Sleep(40)，优化前输入阶段 564ms、全部完成 890ms、尾部 326ms；优化后为 565ms、588ms、23ms。独立滑动由 3 次减到 2 次（剩余一次重落指由屏幕边缘触发）。这是电脑端队列测量，不代表手机实际帧时间或用户感知延迟。
- 已备份原电脑端程序并替换现有迁移目录的程序；手机应用未重新安装。用户在小红书或小黑盒图文页面复测连续及反向滚动后，确认“已经顺畅，基本不卡”。点击暂停问题在上一版已确认恢复正常。
- 修改及测量代码保持本地，未提交或推送。

## 左键拖动停在半路修复（2026-11-09）

- 用户报告：按住鼠标左键滑动时，手机和电脑会同步停在半滑开的状态，松开后仍可能保持；与此前滚轮顿挫分别检查。
- 真机通信复现：单独发送 touchDown、touchMove、touchUp 时，每条均收到一条以换行结尾的 JSON 返回；将最终 touchMove 与 touchUp 拼接发送，只收到一条返回。单纯确认 Windows send 成功不能保证手机处理了松开。测试后另外发送独立松开，释放测试触控。
- src/input_injector.cpp 改为同一连接只保留一条未确认请求，等完整返回（含可能单独到达的换行结束符）后才发送下一条。重连线程不再读取并丢弃返回，避免抢走正在等待的确认。
- 设置确认超时和回复大小边界；无效或缺失确认时关闭失步连接，原有松开失败恢复逻辑尝试 shell 释放。保持点击、拖动、连续滚轮在同一工作线程的顺序。
- tests/input_tests.cpp 增加模拟真实代理截断同批多条请求的后台接收器；加入延迟回复、JSON 分段及结束符单独到达、松开确认缺失时恢复释放的场景。输入回归共 14 项，CTest 输入和音频两个测试组均通过；Release 构建成功。
- 本地构建与当前迁移目录的 hscrcpy.exe SHA-256 均为 379aeb7f05be54403c92ae49a633c04030362dc7895cbc346d67f125c04ea109。旧程序备份保存在本地 .validation/，不进入公开包。
- 用户复测先反馈“顺滑不少”，随后明确确认“已经不会停在半路”。修复已在现有手机和电脑上验证；本轮未重装手机应用。
- 公开源码和完整迁移包同步本次修复；旧压缩包保留。上述验证期间尚未提交或推送；发布结果以 GitHub 仓库与 Releases 为准。
