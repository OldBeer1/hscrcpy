# hscrcpy

Windows 端鸿蒙设备投屏控制工具，Base On [HOScrcpy](https://gitcode.com/OpenHarmonyToolkitsPlaza/HOScrcpy/)。

## 功能特性

- **H264 实时投屏**：通过 gRPC 虚拟屏流 + FFmpeg 硬解，低延迟实时画面
- **JPEG 快照回退**：无 FFmpeg 时自动降级为 `snapshot_display` 模式（~2 FPS）
- **触控反控**：点击、长按、滑动、鼠标滚轮
- **键盘输入**：英文/数字通过 `uinput` keyEvent 映射，中文通过 `uitest uiInput inputText` 输入
- **剪贴板粘贴**：Ctrl+V 将 Windows 剪贴板内容输入到手机
- **多实例**：支持同时打开多个 hscrcpy 窗口，分别连接不同设备
- **USB / TCP 连接**：支持 USB 直连和 WiFi 无线连接

## 系统架构

```
┌─────────────────────────────────────────────────────────┐
│                    Windows Host                          │
│  ┌───────────────────────────────────────────────────┐  │
│  │              hscrcpy.exe (Win32 GUI)               │  │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────────────┐ │  │
│  │  │ Win32    │ │ FFmpeg   │ │  HDC Wrapper      │ │  │
│  │  │ GDI      │ │ H264     │ │  devices/forward  │ │  │
│  │  │ Render   │ │ Decode   │ │  shell/push       │ │  │
│  │  │ Input    │ │          │ │                    │ │  │
│  │  └────┬─────┘ └────┬─────┘ └────────┬──────────┘ │  │
│  │       │            │                │             │  │
│  │  ┌────┴────────────┴────────────────┴──────────┐  │  │
│  │  │            ScreenCapture                     │  │  │
│  │  │  ┌──────────────┐  ┌─────────────────────┐  │  │  │
│  │  │  │ gRPC Stream  │  │ Snapshot Fallback   │  │  │  │
│  │  │  │ (H264)       │  │ (JPEG/GDI+)         │  │  │  │
│  │  │  └──────────────┘  └─────────────────────┘  │  │  │
│  │  └─────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────┘  │
│         │ gRPC (H264)       │ uitest socket (control)   │
│    ┌────┴───────────────────┴──────────┐                │
│    │         hdc fport forward         │                │
│    └────────────┬──────────────────────┘                │
└─────────────────┼───────────────────────────────────────┘
                  │ USB / TCP
┌─────────────────┼───────────────────────────────────────┐
│            HarmonyOS Device                             │
│  ┌──────────────┴──────────────────────────────────┐   │
│  │  uitest extension (libscreen_casting.z.so)       │   │
│  │  VirtualScreen → H264 encode → gRPC stream       │   │
│  └──────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────┐   │
│  │  uitest agent (agent.so)                          │   │
│  │  Touch/Key/Mouse injection via socket JSON        │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## 前置条件

- Windows 10/11
- [HDC](https://developer.huawei.com/consumer/cn/download/) (HarmonyOS Device Connector)，已加入 PATH

## 构建

### 环境要求

- Windows 10/11
- [MSYS2](https://www.msys2.org/) + MinGW-w64 工具链
- **FFmpeg 7.x**（可选，用于 H264 硬解，无 FFmpeg 时自动降级 JPEG 快照模式）

### 编译

```powershell
.\build.ps1
```

首次运行会自动检测 MSYS2 并安装 GCC/CMake/Make，后续编译直接跳过安装步骤。

输出：`build\hscrcpy.exe`

### 清理

```powershell
.\build.ps1 -Clean
```

输出：`build\hscrcpy.exe`

## 使用

### GUI 模式

1. USB 连接鸿蒙设备
2. 运行 `hscrcpy.exe`
3. 从设备列表选择设备
4. 点击 **Start** 开始投屏

### 命令行参数

```
hscrcpy.exe [options]

Options:
  -s, --serial=SERIAL    设备序列号或 TCP 地址 (如 127.0.0.1:5555)
  -a, --auto-start       自动开始投屏（需配合 -s 使用）
  -f, --fps=FPS          帧率 (默认 30)
  -d, --display=ID       显示 ID (默认 0)
  -n, --no-control       禁用反控
  -v, --verbose          详细日志
  --help                 显示帮助
```

### 示例

```cmd
:: USB 设备自动投屏
hscrcpy.exe -a -s FMR0224B14009124

:: WiFi 设备
hscrcpy.exe -s 192.168.1.100:5555
```

## 操作说明

### 鼠标操作

| 操作 | 输入 |
|------|------|
| 点击 | 左键点击 |
| 长按 | 长按左键 (>500ms) |
| 滑动 | 左键拖拽 |
| 返回 | 右键点击 |
| 滚轮 | 鼠标滚轮上下 |

### 键盘操作

| 操作 | 输入 |
|------|------|
| 英文/数字输入 | 键盘字母/数字键 |
| 中文输入 | 输入法 commit 后自动发送 |
| 粘贴 | Ctrl+V |
| 退格 | Backspace |
| 回车 | Enter |
| 方向键 | ↑ ↓ ← → |
| Home | Home 键 |

### 工具栏按钮

**Back** · **Home** · **Recent** · **Vol+** · **Vol-** · **Power** · **Reboot**

## 项目结构

```
hscrcpy/
├── src/                    # 源代码
│   ├── main.cpp            # Win32 GUI 主程序
│   ├── screen_capture.cpp  # 投屏核心（gRPC 流 / 快照回退）
│   ├── grpc_client.cpp     # HTTP/2 + gRPC 客户端（无第三方依赖）
│   ├── h264_decoder.cpp    # FFmpeg H264 解码（条件编译）
│   ├── jpeg_decoder.cpp    # GDI+ JPEG 解码（回退模式）
│   ├── input_injector.cpp  # 输入注入（uitest socket / shell）
│   ├── hdc_wrapper.cpp     # HDC 命令封装
│   ├── cli.cpp             # 命令行参数解析
│   └── window_renderer.cpp # Win32 窗口渲染（备用）
├── common/                 # 公共头文件
│   ├── protocol.h          # 协议常量、端口定义
│   ├── log.h               # 日志宏
│   └── version.h           # 版本号
├── server/                 # 设备端二进制
│   ├── libscrcpy_server*.z.so  # uitest 扩展（虚拟屏 H264 编码）
│   └── uitest_agent*.so        # uitest agent（输入注入）
├── ffmpeg/                 # FFmpeg 开发文件（可选，需自行下载）
├── CMakeLists.txt          # CMake 构建配置
├── build.ps1               # Windows 构建脚本
├── download_ffmpeg.ps1     # FFmpeg 自动下载脚本
└── .gitignore
```

## 技术实现

### 投屏链路

1. 通过 `hdc shell` 推送并启动 uitest 扩展（`libscreen_casting.z.so`）
2. 扩展创建 VirtualScreen，绑定 H264 编码器，通过 gRPC SERVER_STREAMING 推流
3. 客户端通过 `hdc fport` 端口转发连接 gRPC，接收 H264 裸帧
4. FFmpeg 解码为 BGR24，Win32 `StretchDIBits` 渲染

### 输入注入

- **Socket 模式**（低延迟）：通过 uitest agent 的 JSON socket 协议发送 touchDown/touchMove/touchUp
- **Shell 模式**（回退）：通过 `hdc shell uitest uiInput` 命令发送
- **键盘映射**：Windows 虚拟键码 → HarmonyOS 键码，通过 `uinput -K` 发送 keyEvent
- **中文输入**：IME committed text 通过 `uitest uiInput inputText` 发送

### 多实例支持

- 每个 hscrcpy 实例独立管理自己的 fport 转发
- uitest 扩展为共享资源，stop 时不 kill，start 时按需复用（SO MD5 匹配则跳过推送）
- gRPC SERVER_STREAMING 天然支持多客户端并发

## 支持设备

- HarmonyOS NEXT (API 12+)
- uitest 5.x / 6.x
- 已验证：HUAWEI Mate 60 Pro (ALN-AL00)

## 致谢

- [HOScrcpy](https://gitcode.com/OpenHarmonyToolkitsPlaza/HOScrcpy) — 鸿蒙投屏工具SDK
- [scrcpy](https://github.com/Genymobile/scrcpy/) — Android 投屏工具，架构设计参考
- [FFmpeg](https://ffmpeg.org) — H264 视频解码

## License

[Apache License 2.0](LICENSE)