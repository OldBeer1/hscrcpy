# hscrcpy

Windows-to-HarmonyOS screen mirroring tool.

## Prerequisites

- Windows 10/11
- [HDC](https://developer.huawei.com/consumer/cn/download/) (HarmonyOS Device Connector)
- FFmpeg shared DLLs (for H264 video decoding via gRPC virtual screen streaming):
  1. Download `ffmpeg-master-latest-win64-gpl-shared.zip` from https://github.com/BtbN/FFmpeg-Builds/releases
  2. Extract and copy the following DLLs to the same directory as `hscrcpy.exe`:
   - `avcodec-62.dll`
   - `avutil-60.dll`
   - `swscale-9.dll`

> Without FFmpeg DLLs, hscrcpy falls back to `snapshot_display` JPEG capture mode (slow, ~2 FPS).

## Build

```
build.bat
```

Output: `build\hscrcpy.exe`

## Usage

1. Connect HarmonyOS device via USB
2. Run `hscrcpy.exe`
3. Select device from list
4. Click "Start" to begin mirroring

## Controls

| Action | Input |
|--------|-------|
| Click | Left click |
| Long click | Long press left button |
| Swipe | Left drag |
| Back | Right click |
| Volume Up/Down | Mouse wheel |

## Supported Devices

- HarmonyOS NEXT (API 12+)
- uitest 5.x and 6.x
- Verified on HUAWEI Mate 60 Pro (ALN-AL00)
