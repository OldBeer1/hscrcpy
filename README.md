# hscrcpy

Windows 端鸿蒙设备投屏控制工具，Base On [HOScrcpy](https://gitcode.com/OpenHarmonyToolkitsPlaza/HOScrcpy/)。

![image.png](https://raw.gitcode.com/user-images/assets/10037292/b133fdca-4405-4ccf-9d1b-c6468a074831/image.png 'image.png')

## 功能特性

- **H264 实时投屏**：通过 gRPC 虚拟屏流 + nghttp2 + FFmpeg 硬解，低延迟实时画面
- **JPEG 快照回退**：无 FFmpeg 时自动降级为 `snapshot_display` 模式（~2 FPS）
- **Direct2D 渲染**：优先使用 D2D 硬件加速渲染，GDI 回退；零拷贝解码 + 帧池复用
- **UI 布局检查**：实时查看设备 UI 组件树，点击节点高亮定位，支持搜索/刷新/展开，可拖拽分割线调整面板
- **触控反控**：点击、长按、滑动、鼠标滚轮；双通道 touchUp 保证释放；鼠标拖出画面自动释放
- **键盘输入**：英文/数字通过 `uinput` keyEvent 映射，中文通过 `uitest uiInput inputText` 输入
- **剪贴板粘贴**：Ctrl+V 将 Windows 剪贴板内容输入到手机
- **文件管理器**：浏览设备文件系统，支持上传/下载/删除/新建目录
- **云手机模拟面板**：电池状态注入、GNSS 模拟定位，支持 CPH 云手机自动检测
- **多实例**：支持同时打开多个 hscrcpy 窗口，分别连接不同设备
- **USB / TCP 连接**：支持 USB 直连和 WiFi 无线连接
- **参数配置**：独立 USB/TCP 编码参数（缩放、码率、帧率），INI 持久化存储

## 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                    Windows Host                           │
│  ┌────────────────────────────────────────────────────┐  │
│  │              hscrcpy.exe (Win32 GUI)                │  │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────────────┐  │  │
│  │  │ D2D/GDI  │ │ FFmpeg   │ │  HDC Wrapper      │  │  │
│  │  │ Render   │ │ H264     │ │  devices/forward  │  │  │
│  │  │ Layout   │ │ Decode   │ │  shell/push       │  │  │
│  │  │ Input    │ │ nghttp2  │ │                    │  │  │
│  │  └────┬─────┘ └────┬─────┘ └────────┬──────────┘  │  │
│  │       │            │                │              │  │
│  │  ┌────┴────────────┴────────────────┴──────────┐   │  │
│  │  │            ScreenCapture                     │   │  │
│  │  │  ┌──────────────┐  ┌─────────────────────┐  │   │  │
│  │  │  │ gRPC Stream  │  │ Snapshot Fallback   │  │   │  │
│  │  │  │ (nghttp2)    │  │ (JPEG/GDI+)         │  │   │  │
│  │  │  └──────────────┘  └─────────────────────┘  │   │  │
│  │  └─────────────────────────────────────────────┘   │  │
│  └────────────────────────────────────────────────────┘  │
│         │ gRPC (H264)       │ uitest socket (control)    │
│    ┌────┴───────────────────┴──────────┐                 │
│    │         hdc fport forward         │                 │
│    └────────────┬──────────────────────┘                 │
└─────────────────┼────────────────────────────────────────┘
                  │ USB / TCP
┌─────────────────┼────────────────────────────────────────┐
│            HarmonyOS Device                              │
│  ┌──────────────┴──────────────────────────────────┐    │
│  │  uitest extension (libscreen_casting.z.so)       │    │
│  │  VirtualScreen → H264 encode → gRPC stream       │    │
│  └──────────────────────────────────────────────────┘    │
│  ┌──────────────────────────────────────────────────┐    │
│  │  uitest agent (agent.so)                          │    │
│  │  Touch/Key/Mouse injection via socket JSON        │    │
│  └──────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────┘
```

## 前置条件

- Windows 10/11
- [HDC](https://developer.huawei.com/consumer/cn/download/) (HarmonyOS Device Connector)，已加入 PATH

## 构建

### 环境要求

- Windows 10/11
- [MSYS2](https://www.msys2.org/) + MinGW-w64 工具链
- **nghttp2**（构建脚本自动安装，用于 gRPC HTTP/2 协议）
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

| 按钮 | 功能 |
|------|------|
| ⚙ Settings | 打开参数设置（缩放、码率、帧率、端口等） |
| 📐 Layout | 打开/关闭 UI 布局检查面板 |
| 📁 Files | 打开/关闭文件管理器面板 |
| ☁ CPH | 打开/关闭云手机模拟面板（电池/GNSS） |
| Back | 返回键 |
| Home | 主页键 |
| Recent | 最近任务键 |
| Vol+ / Vol- | 音量调节 |
| Power | 电源键 |
| Reboot | 重启设备 |

## 项目结构

```
hscrcpy/
├── src/                    # 源代码
│   ├── main.cpp            # Win32 GUI 主程序
│   ├── screen_capture.cpp  # 投屏核心（gRPC 流 / 快照回退）
│   ├── grpc_client.cpp     # nghttp2 + gRPC 客户端
│   ├── h264_decoder.cpp    # FFmpeg H264 解码（条件编译）
│   ├── jpeg_decoder.cpp    # GDI+ JPEG 解码（回退模式）
│   ├── input_injector.cpp  # 输入注入（uitest socket / shell，异步队列）
│   ├── hdc_wrapper.cpp     # HDC 命令封装
│   ├── layout_window.cpp   # UI 布局检查面板（TreeView + ListView）
│   ├── file_manager_window.cpp  # 文件管理器面板
│   ├── cph_window.cpp      # 云手机模拟面板（电池/GNSS）
│   ├── config.cpp          # INI 配置读写
│   ├── cli.cpp             # 命令行参数解析
│   └── window_renderer.cpp # Win32 窗口渲染（备用）
├── common/                 # 公共头文件
│   ├── protocol.h          # 协议常量、端口定义
│   ├── log.h               # 日志宏
│   ├── net_util.h          # 网络工具（TCP keepalive 等）
│   └── version.h           # 版本号
├── res/                    # 资源文件
│   ├── settings_icon.png   # 设置按钮图标
│   └── layout_icon.png     # 布局按钮图标
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
3. 客户端通过 `hdc fport` 端口转发连接 gRPC，nghttp2 处理 HTTP/2 帧解析，接收 H264 裸帧
4. FFmpeg 解码为 BGRA，Direct2D 硬件加速渲染（GDI StretchDIBits 回退）

### 渲染管线

- **D2D 路径**（优先）：`ID2D1HwndRenderTarget` + `CreateBitmap` + `CopyFromMemory` + `DrawBitmap`
- **GDI 路径**（回退）：`CreateCompatibleDC` + `StretchDIBits` + `BitBlt`
- **帧池复用**：解码线程与渲染线程通过 `shared_ptr` swap 零拷贝传递帧数据
- **事件驱动**：解码完成立即 `SetEvent` 通知 UI 线程渲染，避免轮询延迟

### UI 布局检查

- 通过 `uitest dumpLayout` 获取设备 UI 树 JSON，`hdc file recv` 拉取到本地解析
- TreeView 展示组件层级，ListView 显示属性详情（type/text/id/bounds/center）
- 选中节点在投屏画面上绘制半透明蓝色高亮矩形（D2D/GDI 双路径）
- 分割线可拖拽调整面板宽度，搜索支持按 type/text/id/key/description 定位

### 输入注入

- **Socket 模式**（低延迟）：通过 uitest agent 的 JSON socket 协议发送 touchDown/touchMove/touchUp
- **Shell 模式**（回退）：通过 `hdc shell uinput` 命令发送，所有 socket 方法均有 shell fallback
- **双通道 touchUp**：touchUp 同时走 socket（快速）和 shell（保证送达），避免手指卡住
- **异步队列**：所有注入方法通过 task_queue 异步执行，不阻塞 UI 线程
- **16ms 限流**：touchMove 事件限流至 ~60Hz，避免队列积压
- **鼠标离开画面自动释放**：拖拽超出画面区域时自动发送 touchUp
- **键盘映射**：Windows 虚拟键码 → HarmonyOS 键码，通过 `uinput -K` 发送 keyEvent
- **中文输入**：IME committed text 通过 `uitest uiInput inputText` 发送

### 多实例支持

- 每个 hscrcpy 实例独立管理自己的 fport 转发
- uitest 扩展为共享资源，stop 时不 kill，start 时按需复用（版本优先匹配 SO）
- gRPC SERVER_STREAMING 天然支持多客户端并发

## 支持设备

- HarmonyOS NEXT (API 12+)
- uitest 5.x / 6.x
- 已验证：HUAWEI Mate 60 Pro (ALN-AL00)

## 致谢

- [HOScrcpy](https://gitcode.com/OpenHarmonyToolkitsPlaza/HOScrcpy) — 鸿蒙投屏工具SDK
- [scrcpy](https://github.com/Genymobile/scrcpy/) — Android 投屏工具，架构设计参考
- [FFmpeg](https://ffmpeg.org) — H264 视频解码
- [nghttp2](https://nghttp2.org) — HTTP/2 协议实现

## License

[Apache License 2.0](LICENSE)
