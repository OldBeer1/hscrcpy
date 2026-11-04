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

当前机器未配置 HDC，因此未连接手机，未进行鸿蒙 7 真机投屏、反控、录像和长时间稳定性测试。编译通过不代表所有设备兼容性已验证，也不代表已增加音频转发功能。

## 本地目录

- `.tools/`：便携工具和依赖，仅供本项目使用。
- `.validation/`：编译及启动检查日志。
- `build/hscrcpy.exe`：本机 Release 构建产物。

本次便携构建使用 `MinGW Makefiles` 生成器，nghttp2 安装到 `.tools/nghttp2-local/ucrt64`，并通过 CMake 参数 `NGHTTP2_ROOT` 指定 `.tools/nghttp2-local`。原 `build.ps1` 仍面向 MSYS2，不会自动识别这套便携工具。
