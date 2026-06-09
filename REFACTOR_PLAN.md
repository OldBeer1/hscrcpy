# HScrcpy 性能优化整改方案

## 概述

本文档梳理 hscrcpy 项目当前的性能瓶颈，并制定具体的整改计划。优化目标：**降低 CPU 占用、提高帧率稳定性、减少输入延迟**。

---

## 优化项总览

| # | 优化项 | 模块 | 预期收益 | 工作量 | 优先级 |
|---|--------|------|----------|--------|--------|
| 1 | 键盘事件改用 socket 协议注入 | input_injector | 消除每次按键的进程创建开销 | 小 | 🔴 P0 |
| 2 | 持久化双缓冲渲染 | main (WM_PAINT) | 消除每帧 GDI 资源创建/销毁 | 小 | 🔴 P0 |
| 3 | 事件驱动渲染替代固定轮询 | main (WM_TIMER) | 减少 UI 线程无谓唤醒（60次/s） | 小 | 🔴 P0 |
| 4 | 解码线程数动态化 | h264_decoder | 充分利用多核 CPU | 小 | 🟡 P1 |
| 5 | 消除 H.264 解码多余 memcpy | h264_decoder | 减少每帧内存拷贝 | 小 | 🟡 P1 |
| 6 | 全局状态封装 | main | 提高可维护性、线程安全 | 中 | 🟡 P1 |
| 7 | detach 线程改为 joinable | main | 避免资源泄露、可控清理 | 中 | 🟢 P2 |

---

## 详细方案

### 1. 键盘事件改用 socket 协议 (P0)

**现状**: `inject_key_code()`, `inject_back()`, `inject_home()`, `inject_recent()`, `inject_power()`, `inject_key_event()` 全部通过 `hdc_.shell_command()` 执行，每次按键启动一个 `cmd.exe /C hdc ...` 进程（~10-50ms）。

**方案**:
- 在 socket 模式下，将键盘事件通过 uitest agent 的 JSON 协议发送，与触摸事件走同一通道
- 在 `InputInjector` 中添加 `inject_key_event_socket(key_code, action)` 方法
- socket 发送失败时自动回退到 shell 命令

**关键代码**: `input_injector.cpp:390-423`

```cpp
// 当前 (shell 模式):
bool inject_back() {
    return hdc_.shell_command("uinput -K -d 2 -u 2", nullptr, 3);
}

// 改为 (socket 优先):
bool inject_back() {
    if (use_socket_mode_) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"module\":\"...\",\"method\":\"KeyEvent\","
                 "\"params\":{\"api\":\"keyCode\",\"args\":{\"code\":2}}}");
        if (send_to_socket(json)) return true;
    }
    return hdc_.shell_command("uinput -K -d 2 -u 2", nullptr, 3);
}
```

### 2. 持久化双缓冲 (P0)

**现状**: `WM_PAINT` 每帧调用 `CreateCompatibleDC` + `CreateCompatibleBitmap` + `SelectObject` -> 渲染 -> `DeleteObject` + `DeleteDC`，每帧都有堆分配和 GDI 对象创建/销毁。

**方案**:
- 在窗口创建/大小变化时创建持久化的内存 DC 和兼容位图
- 在 `WM_SIZE` 和 `WM_PAINT` 中复用，仅当窗口大小变化时重建
- 消除 `WM_ERASEBKGND` 的背景擦除

### 3. 事件驱动渲染 (P0)

**现状**: `SetTimer(hwnd, 1, 16, NULL)` 每 16ms 触发 `WM_TIMER`，调用 `try_update_frame()` 比对帧序号。60 次/秒的轮询即使无新帧也唤醒 UI 线程。

**方案**:
- 移除固定 16ms 定时器
- 在 `screen_capture.cpp` 的 `decode_loop` 设置帧事件后，通过 `PostMessage` 通知 UI 线程
- UI 线程收到消息后直接取最新帧并 `InvalidateRect`

### 4. 解码线程数动态化 (P1)

**现状**: `codec_ctx_->thread_count = 2` 硬编码。

**方案**: `thread_count = std::max(2, std::thread::hardware_concurrency() / 2)`

### 5. 消除多余 memcpy (P1)

**现状**: 解码流程: `sws_scale` (YUV→BGR) → `memcpy` (BGR→`image_.data`)，二次拷贝。

**方案**:
- 直接使用 `frame_rgb_->data[0]` / `frame_rgb_->linesize[0]` 作为 `DecodedImage` 的数据源
- 或者改用 `av_frame_make_writable` + 共享引用计数，避免额外拷贝

### 6. 全局状态封装 (P1)

**现状**: `main.cpp` 顶部 ~30 个 `static` 全局变量。

**方案**: 封装为 `AppState` 类，所有状态通过实例访问，降低耦合、便于测试。

### 7. detach 线程改为 joinable (P2)

**现状**: `stop_mirroring()` 和新线程用 `.detach()` 执行清理。

**方案**: 改用 `std::packaged_task` + `std::future` 或事件同步，确保线程完成后再继续。

---

## 里程碑划分

- **Phase 1 (P0)** — 输入注入 + 渲染优化 + 事件驱动 (高收益、低风险)
- **Phase 2 (P1)** — 解码优化 + 状态封装 (中等收益)
- **Phase 3 (P2)** — 线程模型改进 (低优先)
