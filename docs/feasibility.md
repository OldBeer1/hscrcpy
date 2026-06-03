# HScrcpy 可行性分析报告

## 一、项目概述

HScrcpy 是一款 Windows 对鸿蒙（OpenHarmony/HarmonyOS）设备的虚拟屏投屏与反控工具，核心对标 Android 平台的 Scrcpy 和鸿蒙平台的 HOScrcpy。

**核心链路**：Windows 客户端 ←→ HDC 端口转发 ←→ 鸿蒙设备侧 hscrcpy-server

## 二、技术可行性分析

### 2.1 鸿蒙端关键 API 可行性

| 能力 | API | 可行性 | 风险等级 |
|------|-----|--------|----------|
| 虚拟屏创建 | `VirtualScreenManager::CreateVirtualScreen()` | ✅ OpenHarmony NDK 已提供 | 低 |
| 虚拟屏 Surface 获取 | `VirtualScreenManager::GetSurface()` | ✅ 配套 API | 低 |
| 硬件编码 H265 | `OH_AVCodec_CreateByMime("video/hevc", true)` | ✅ NDK AVCodec 支持 Surface 模式 | 低 |
| Surface 绑定编码器 | `OH_VideoEncoder_SetSurface()` | ✅ NDK 已支持 | 低 |
| TCP 通信 | POSIX socket | ✅ 标准 C 库 | 无 |
| 触控事件注入 | `InputManager::InjectTouchEvent()` | ⚠️ 需要 Inner API / 系统签名 | 中 |
| 按键事件注入 | `InputManager::InjectKeyEvent()` | ⚠️ 需要 Inner API / 系统签名 | 中 |

**结论**：视频采集+编码+出流链路完全可行，输入注入依赖系统权限。

### 2.2 Windows 客户端可行性

| 能力 | 技术选型 | 可行性 | 风险等级 |
|------|----------|--------|----------|
| H265 解码 | FFmpeg libavcodec (软解 + D3D11VA 硬解) | ✅ 成熟方案 | 无 |
| 窗口渲染 | SDL3 | ✅ 跨平台成熟库 | 无 |
| 键鼠捕获 | SDL3 事件系统 | ✅ 原生支持 | 无 |
| HDC 通信 | 调用 hdc.exe 子进程 | ✅ 与 Scrcpy 调用 adb 方式一致 | 无 |
| TCP 连接 | Winsock2 / POSIX socket | ✅ 标准网络编程 | 无 |

**结论**：Windows 客户端所有组件均为成熟技术，可行性高。

### 2.3 与 Scrcpy 架构对比

| 维度 | Scrcpy (Android) | HScrcpy (鸿蒙) | 差异 |
|------|-------------------|----------------|------|
| 设备通信 | adb forward | hdc forward | 对等替换 |
| 屏幕采集 | MediaProjection / VirtualDisplay | VirtualScreen + Surface | 架构一致 |
| 视频编码 | MediaCodec H264/H265 | OH_AVCodec H265 | 架构一致 |
| 服务端运行 | `app_process` 加载 jar | 原生 C/C++ 二进制 | HScrcpy 更轻量 |
| 输入注入 | InputManager (shell 权限) | InputManager (系统签名) | 权限模型不同 |
| 客户端 | SDL2 + FFmpeg + libusb | SDL3 + FFmpeg + hdc.exe | 技术栈升级 |

**结论**：架构高度对齐 Scrcpy，核心链路可完整复刻。

### 2.4 与 HOScrcpy 对比

| 维度 | HOScrcpy | HScrcpy | 差异 |
|------|----------|---------|------|
| 服务端语言 | Python + HAP | C/C++ 原生二进制 | HScrcpy 无运行时依赖 |
| 屏幕采集 | 截屏流 / 录屏 API | VirtualScreen 直出 | HScrcpy 零拷贝更高效 |
| 反控方式 | HDC 命令 / UiTest | TCP 实时注入 | HScrcpy 延迟更低 |
| 部署方式 | 安装 HAP | 推送二进制 | HScrcpy 更轻量 |

**结论**：HScrcpy 在性能和部署便捷性上优于 HOScrcpy。

## 三、关键风险与缓解

### 3.1 权限风险（高风险）

**问题**：`MANAGE_VIRTUAL_SCREEN` 和 `INPUT_CONTROL` 为系统级权限，普通应用无法获取。

**缓解**：
- 目标场景为云手机/定制 ROM，具备系统签名条件
- 服务端二进制推送到 `/data/local/tmp/`，以 shell 权限运行可绕过部分限制
- 后续可探索通过 HAP 系统应用方式签名打包

### 3.2 Inner API 稳定性风险（中风险）

**问题**：`InputManager::InjectTouchEvent()` 等可能属于 Inner API，版本间可能变化。

**缓解**：
- 封装输入注入层，隔离 API 变化
- 提供多版本适配（条件编译）
- 降级方案：通过 hdc shell input 命令注入（延迟高但兼容性好）

### 3.3 编码兼容性风险（低风险）

**问题**：不同芯片平台的硬件编码器能力差异。

**缓解**：
- 运行时检测编码器能力，动态选择 H265/H264
- 提供软编码降级路径
- 参数可配置（分辨率、帧率、码率）

### 3.4 跨平台扩展风险（低风险）

**问题**：后续扩展到 Ubuntu Linux 平台。

**缓解**：
- 客户端使用 SDL3 + FFmpeg，天然跨平台
- HDC 通信层抽象为接口，Linux 下可替换为 hdc Linux 版本
- 网络层使用 POSIX socket，Windows/Linux 双平台兼容

## 四、性能预估

| 指标 | 预估值 | 依据 |
|------|--------|------|
| 端到端延迟 | 30-80ms | Scrcpy 同架构实测 30-70ms |
| 帧率 | 30-60fps | 硬编码器支持 |
| 编码延迟 | <10ms | H265 硬编码 |
| 网络传输 | <5ms | USB 本地转发 |
| 解码延迟 | <10ms | D3D11VA 硬解 |
| 渲染延迟 | <5ms | SDL3 GPU 渲染 |

## 五、可行性结论

**项目整体可行性：高**

1. 核心链路（虚拟屏 → 硬编码 → TCP 出流 → 客户端解码渲染）在 OpenHarmony NDK 层面完全可行
2. Windows 客户端技术栈（SDL3 + FFmpeg）成熟可靠
3. 架构对标 Scrcpy 已验证方案，风险可控
4. 主要限制在于设备权限（系统签名），但目标场景（云手机/定制 ROM）可满足
5. 跨平台扩展（Linux）因技术选型天然具备基础

**建议**：按分步迭代策略，先通画面（最小 MVP），再迭代控制、音频、剪贴板等功能。
