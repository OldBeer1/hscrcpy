# HScrcpy 开发指南

## 环境要求

### 客户端 (Windows)

- Windows 10/11
- CMake 3.16+
- Visual Studio 2022 或 MinGW-w64
- SDL3 开发库
- FFmpeg 6.x+ 开发库 (avcodec, avformat, avutil, swscale)
- hdc.exe (HarmonyOS Device Connector)

### 服务端 (鸿蒙)

- OpenHarmony NDK (API 12+)
- CMake 3.16+
- 目标设备需系统签名权限

## 构建

### 客户端

```bash
mkdir build && cd build
cmake .. -DBUILD_CLIENT=ON
cmake --build . --config Release -j
```

### 服务端

```bash
mkdir build && cd build
cmake .. -DBUILD_SERVER=ON -DOHOS_NDK=/path/to/ohos-ndk
cmake --build . --config Release -j
```

## 使用

### 一键启动

```bat
scripts\start.bat
```

### 手动步骤

```bat
# 1. 推送服务端到设备
hdc push hscrcpy-server /data/local/tmp/
hdc shell chmod 755 /data/local/tmp/hscrcpy-server

# 2. 端口转发
hdc forward tcp:8888 local:8888
hdc forward tcp:8889 local:8889

# 3. 启动服务端
hdc shell /data/local/tmp/hscrcpy-server &

# 4. 启动客户端
hscrcpy.exe
```

### 命令行参数

```
hscrcpy.exe [options]

Options:
  -s, --serial=SERIAL    设备序列号
  -w, --width=WIDTH      虚拟屏宽度 (默认 1080)
  -h, --height=HEIGHT    虚拟屏高度 (默认 1920)
  -f, --fps=FPS          帧率 (默认 30)
  -b, --bitrate=BPS      码率 (默认 8M)
  -n, --no-control       禁用反控
  --codec=CODEC          编码器 h265/h264 (默认 h265)
  --help                 显示帮助
```
