# HScrcpy 架构设计文档

## 一、系统架构总览

```
┌─────────────────────────────────────────────────────────┐
│                   Windows Host                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │              hscrcpy.exe (Client)                 │   │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │   │
│  │  │  SDL3    │ │  FFmpeg  │ │  HDC Controller  │ │   │
│  │  │ Window   │ │ Decoder  │ │  (hdc.exe wrapper)│ │   │
│  │  │ Render   │ │ H265     │ │  devices/forward  │ │   │
│  │  │ Input    │ │ D3D11VA  │ │  shell/start     │ │   │
│  │  └────┬─────┘ └────┬─────┘ └────────┬─────────┘ │   │
│  │       │            │                │            │   │
│  │  ┌────┴────────────┴────────────────┴─────────┐  │   │
│  │  │              Core Engine                    │  │   │
│  │  │  ┌──────────┐ ┌──────────┐ ┌────────────┐ │  │   │
│  │  │  │ Video    │ │ Control  │ │  Device    │ │  │   │
│  │  │  │ Receiver │ │ Sender   │ │  Manager   │ │  │   │
│  │  │  │ (TCP)    │ │ (TCP)    │ │            │ │  │   │
│  │  │  └──────────┘ └──────────┘ └────────────┘ │  │   │
│  │  └────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────┘   │
│         │ TCP:8888 (video)    │ TCP:8889 (control)      │
│         │                    │                          │
│    ┌────┴────────────────────┴────┐                     │
│    │      hdc forward             │                     │
│    │  tcp:8888 → local:8888       │                     │
│    │  tcp:8889 → local:8889       │                     │
│    └────────────┬─────────────────┘                     │
└─────────────────┼───────────────────────────────────────┘
                  │ USB / WiFi (HDC)
┌─────────────────┼───────────────────────────────────────┐
│            HarmonyOS Device                              │
│  ┌──────────────┴────────────────────────────────────┐  │
│  │         hscrcpy-server (Native Binary)             │  │
│  │                                                    │  │
│  │  ┌──────────────────────────────────────────────┐ │  │
│  │  │           Video Pipeline                     │ │  │
│  │  │  VirtualScreen → Surface → OH_AVCodec(H265)  │ │  │
│  │  │       → TCP Server :8888 → Socket Send       │ │  │
│  │  └──────────────────────────────────────────────┘ │  │
│  │  ┌──────────────────────────────────────────────┐ │  │
│  │  │           Control Pipeline                   │ │  │
│  │  │  TCP Server :8889 → Protocol Parse           │ │  │
│  │  │       → InputManager::Inject*()              │ │  │
│  │  └──────────────────────────────────────────────┘ │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

## 二、模块设计

### 2.1 鸿蒙端 hscrcpy-server

#### 2.1.1 模块划分

```
server/
├── main.c                  # 入口，参数解析，信号处理
├── virtual_screen.c/h      # 虚拟屏创建/销毁/Surface获取
├── video_encoder.c/h       # OH_AVCodec H265 编码器封装
├── video_server.c/h        # TCP 视频流服务器 (:8888)
├── control_server.c/h      # TCP 控制指令服务器 (:8889)
├── input_injector.c/h      # 输入事件注入（触控/按键）
├── protocol.h              # 控制协议定义
├── net_common.c/h          # 网络工具（socket 创建/接受/发送）
└── CMakeLists.txt
```

#### 2.1.2 核心流程

**视频采集编码出流**：
```
1. CreateVirtualScreen("hscrcpy_vdisp", w, h, dpi)
2. GetSurface(virtualDispId) → encoderSurface
3. OH_VideoEncoder_CreateByMime("video/hevc")
4. OH_VideoEncoder_SetSurface(codec, encoderSurface)
5. OH_VideoEncoder_Configure(codec, config)
6. OH_VideoEncoder_Start(codec)
7. OnNewOutputData callback → send(socket, data, len)
```

**控制指令接收注入**：
```
1. TCP Listen :8889
2. Accept client connection
3. Recv control packet → parse type
4. Switch(type):
   0x01 → InjectTouchEvent(x, y, action)
   0x02 → InjectKeyEvent(keycode, press)
   0x03 → RecreateVirtualScreen(w, h)
   0x04 → SetClipboard(text)
```

#### 2.1.3 编码参数

| 参数 | 默认值 | 可配置 |
|------|--------|--------|
| 编码格式 | H265 (HEVC) | H264 降级 |
| 分辨率 | 1080x1920 | 客户端参数下发 |
| 帧率 | 30fps | 30/60 |
| 码率模式 | CBR | CBR/VBR/CRF |
| 码率 | 8Mbps | 4-20Mbps |
| I帧间隔 | 3s | 1-10s |

### 2.2 Windows 客户端 hscrcpy.exe

#### 2.2.1 模块划分

```
client/
├── main.c                  # 入口，命令行参数解析
├── device_manager.c/h      # HDC 设备枚举/端口转发/服务启动
├── video_receiver.c/h      # TCP 视频流接收 + FFmpeg 解码
├── video_renderer.c/h      # SDL3 窗口创建 + GPU 渲染
├── control_sender.c/h      # 键鼠事件捕获 + 控制协议编码发送
├── input_converter.c/h     # SDL 输入事件 → 鸿蒙协议转换
├── hdc_wrapper.c/h         # hdc.exe 子进程调用封装
├── cli.c/h                 # 命令行参数解析
├── protocol.h              # 控制协议定义（与服务端共享）
├── net_common.c/h          # 网络工具
└── CMakeLists.txt
```

#### 2.2.2 核心流程

```
1. 解析命令行参数 (-s serial -w 1280 -h 720 ...)
2. DeviceManager: hdc devices → 选择设备
3. DeviceManager: hdc forward tcp:8888 local:8888
4. DeviceManager: hdc forward tcp:8889 local:8889
5. DeviceManager: hdc shell /data/local/tmp/hscrcpy-server &
6. VideoReceiver: connect 127.0.0.1:8888
7. ControlSender: connect 127.0.0.1:8889
8. FFmpeg: 打开 H265 解码器（优先 D3D11VA 硬解）
9. SDL3: 创建窗口 + 渲染器
10. 主循环:
    a. VideoReceiver 收帧 → FFmpeg 解码 → SDL 渲染
    b. SDL 事件 → InputConverter → ControlSender 发送
```

#### 2.2.3 解码渲染管线

```
TCP H265 裸流
  → FFmpeg avformat_open_input("tcp://127.0.0.1:8888")
  → av_read_frame() → AVPacket
  → avcodec_send_packet() → avcodec_receive_frame() → AVFrame
  → SDL_RenderTexture() / SDL_UpdateTexture() + SDL_RenderPresent()
```

硬解优先策略：
1. 尝试 D3D11VA → 成功则 GPU 解码 + GPU 渲染
2. D3D11VA 失败 → 软解码 → SDL_UpdateYUVTexture

### 2.3 共享协议模块

```
common/
├── protocol.h              # 控制通道协议定义
└── version.h               # 版本号定义
```

## 三、控制协议设计

### 3.1 视频通道 (:8888)

- 传输内容：H265 裸流（Annex-B 格式）
- 连接方式：TCP 长连接
- 数据格式：编码器直接输出 NAL 单元

### 3.2 控制通道 (:8889)

所有字段采用大端序（Network Byte Order）。

#### 3.2.1 消息头

```
Offset  Size  Field
0       1B    Type    消息类型
```

#### 3.2.2 触控事件 (Type=0x01)

```
Offset  Size  Field
0       1B    Type = 0x01
1       4B    X 坐标 (uint32, 像素)
5       4B    Y 坐标 (uint32, 像素)
9       1B    Action (0:DOWN 1:MOVE 2:UP)
Total: 10 bytes
```

#### 3.2.3 按键事件 (Type=0x02)

```
Offset  Size  Field
0       1B    Type = 0x02
1       2B    KeyCode (uint16, 鸿蒙键码)
3       1B    Action (0:DOWN 1:UP)
Total: 4 bytes
```

#### 3.2.4 分辨率变更 (Type=0x03)

```
Offset  Size  Field
0       1B    Type = 0x03
1       4B    Width (uint32)
5       4B    Height (uint32)
Total: 9 bytes
```

#### 3.2.5 剪贴板同步 (Type=0x04)

```
Offset  Size  Field
0       1B    Type = 0x04
1       4B    Length (uint32, 字节数)
5       NB    Text (UTF-8)
Total: 5 + N bytes
```

#### 3.2.6 滚轮事件 (Type=0x05)

```
Offset  Size  Field
0       1B    Type = 0x05
1       4B    X 坐标
5       4B    Y 坐标
9       4B    ScrollDelta (int32, 正=上, 负=下)
Total: 13 bytes
```

#### 3.2.7 返回键 (Type=0x06)

```
Offset  Size  Field
0       1B    Type = 0x06
Total: 1 byte (快捷键，等效按键事件封装)
```

## 四、工程目录结构

```
hscrcpy/
├── CMakeLists.txt              # 顶层 CMake
├── README.md
├── docs/
│   ├── feasibility.md          # 可行性分析
│   ├── architecture.md         # 架构设计（本文件）
│   └── development.md          # 开发指南
├── common/
│   ├── protocol.h              # 共享协议定义
│   └── version.h               # 版本号
├── server/                     # 鸿蒙端 hscrcpy-server
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── virtual_screen.c/h
│   ├── video_encoder.c/h
│   ├── video_server.c/h
│   ├── control_server.c/h
│   ├── input_injector.c/h
│   └── net_common.c/h
├── client/                     # Windows/Linux 客户端
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── device_manager.c/h
│   ├── video_receiver.c/h
│   ├── video_renderer.c/h
│   ├── control_sender.c/h
│   ├── input_converter.c/h
│   ├── hdc_wrapper.c/h
│   ├── cli.c/h
│   └── net_common.c/h
├── scripts/
│   ├── start.bat               # Windows 一键启动脚本
│   └── build.bat               # Windows 构建脚本
└── third_party/                # 第三方依赖（预编译或源码）
    ├── SDL3/
    └── FFmpeg/
```

## 五、编译构建设计

### 5.1 客户端 (Windows)

- **编译器**：MSVC (Visual Studio 2022) 或 MinGW-w64
- **CMake 最低版本**：3.16
- **依赖**：
  - SDL3（静态链接或 DLL 旁加载）
  - FFmpeg 6.x+（avcodec, avformat, avutil, swscale）
  - Winsock2（系统库）
- **产出**：hscrcpy.exe 单文件（静态链接 SDL/FFmpeg）

### 5.2 服务端 (鸿蒙)

- **编译器**：OHOS NDK 交叉编译工具链 (clang)
- **CMake 最低版本**：3.16
- **依赖**：
  - OpenHarmony NDK（OH_AVCodec, VirtualScreenManager 等）
  - POSIX socket（系统库）
- **产出**：hscrcpy-server 静态链接二进制

### 5.3 编译优化

- CMake 子模块拆分，支持 `-j` 并行编译
- 客户端/服务端独立构建目录
- 预编译头文件加速编译

## 六、跨平台扩展设计

### 6.1 Linux 客户端适配

| 模块 | Windows | Linux |
|------|---------|-------|
| 网络层 | Winsock2 | POSIX socket |
| HDC 通信 | hdc.exe | hdc (Linux) |
| 渲染 | SDL3 + D3D11VA | SDL3 + VAAPI |
| 子进程 | CreateProcess | fork/exec |
| 编译 | MSVC/MinGW | GCC/Clang |

### 6.2 平台抽象层

```c
// platform.h
#ifdef _WIN32
  #define PLATFORM_WINDOWS 1
  #include <winsock2.h>
  typedef SOCKET socket_t;
  #define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
  #define PLATFORM_LINUX 1
  #include <sys/socket.h>
  typedef int socket_t;
  #define INVALID_SOCKET_VALUE (-1)
#endif
```

## 七、分步实施计划

### Phase 1: 最小 MVP（画面通）

1. 服务端：虚拟屏创建 + H265 编码 + TCP 出流
2. 客户端：FFmpeg 解码 + SDL3 显示
3. 启动脚本：hdc forward + 启动服务 + 拉起客户端

### Phase 2: 反控能力

4. 服务端：控制通道 + 输入注入
5. 客户端：键鼠捕获 + 协议编码发送

### Phase 3: 功能增强

6. 分辨率动态调整
7. 剪贴板同步
8. 文件拖拽推送
9. 音频采集传输

### Phase 4: 打包发布

10. 单 exe 打包（内嵌 hdc.exe）
11. Linux 客户端适配
12. 安装脚本/绿色版
