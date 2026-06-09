# HScrcpy 性能优化整改方案

## 概述

本文档梳理 hscrcpy 项目当前的性能瓶颈，并制定具体的整改计划。优化目标：**降低 CPU 占用、提高帧率稳定性、减少输入延迟、支持多实例场景**。

---

## Phase 1 — 已完成 (commit on `develop`)

| # | 优化项 | 模块 | 具体改进 |
|---|--------|------|----------|
| 1 | 键盘事件 socket 协议注入 | `input_injector` | `inject_back/home/recent/power/key_code/key_event/text` 全部先尝试 socket，失败才回退 shell，消除每次按键的进程创建开销 |
| 2 | 持久化双缓冲渲染 | `main.cpp` | `WM_PAINT` 不再每帧创建/销毁 DC+Bitmap，改用 `ensure_backbuffer` 管理的持久缓冲，窗口大小变化时重建 |
| 3 | 解码线程数动态化 | `h264_decoder` | `thread_count = max(1, min(hw_threads/4, 4))`，保守策略适配多实例场景 |
| 4 | 消除 decode buffer 重分配 | `h264_decoder` | 新增 `decode_buffer_` 持久缓冲区，`decode()` 后通过 `vector::swap` 传入 `image_`，避免每帧 reallocate |
| 5 | detach → joinable 线程 | `main.cpp` | 所有 `detach()` 替换为 `g_cleanup_thread`/`g_startup_thread` 跟踪管理，`WM_DESTROY` 时 join，消除竞态 |

---

## Phase 2 — 待实施

### 🔴 P0 — 关键

| # | 问题 | 文件:行 | 当前状态 | 方案 | 预期收益 |
|---|------|---------|----------|------|----------|
| **P0-1** | 编译优化: 无 `-O2`/`-march=native`/`-flto` | `CMakeLists.txt` | MinGW GCC 默认 `-O0`，无 LTO | 添加 `-O2 -march=native -flto` | **全局 CPU 20-50% 提升** |
| **P0-2** | 渲染 `HALFTONE` 拉伸模式极慢 | `main.cpp:660` | `SetStretchBltMode(memdc, HALFTONE)` 使用 4x4 像素平均滤波 | 改用 `COLORONCOLOR`（快速插值，视觉可接受） | **缩放耗时减少 5-15ms/帧** |
| **P0-3** | 每次 `shell_command` spawn `cmd.exe /C hdc` 进程 | `hdc_wrapper.cpp` | 每个 shell 命令创建管道+进程+线程，然后销毁 | 维护持久 `hdc shell` 会话（stdin/stdout 管道） | **消除所有进程创建开销**，snapshot 模式提升 90% |

### 🟡 P1 — 重要

| # | 问题 | 文件:行 | 当前状态 | 方案 |
|---|------|---------|----------|------|
| **P1-1** | gRPC 热路径每帧分配临时 vector | `grpc_client.cpp:573` | `extract_h264_from_reply` 返回 `vector<uint8_t>` 拷贝 | 改用 output-parameter + 预分配 buffer |
| **P1-2** | protobuf 解析器每字段返回 vector 拷贝 | `grpc_client.cpp:190-195` | `read_bytes()` lambda 返回 `vector<uint8_t>`（堆分配+拷贝+释放） | 改为 `read_bytes_into(vector&)` 输出参数 |
| **P1-3** | `memmove` 每迭代移动整个 recv buffer | `grpc_client.cpp:373,549,629` | 消费后 `memmove(buffer, buffer+pos, len)` O(n) 搬移 | 改用环形缓冲区（read/write cursor） |
| **P1-4** | H264 队列 `std::deque` 全量拷贝帧数据 | `screen_capture.cpp:682-687` | `deque.push_back(h264_data)` 拷贝 50-200KB/帧 | 改用 `deque<shared_ptr<vector>>` + `std::move` |
| **P1-5** | `send_to_socket` 不必要的 socket_mutex_ 锁 | `input_injector.cpp:226-247` | 每次 touch move 都加解锁 mutex | 移除 mutex，`send()` 本身线程安全 |
| **P1-6** | `select()` 3s 超时 + 300s 空闲检测 | `grpc_client.cpp:497-513` | idle kill count = 100，需要 300s 检测断连 | 超时降至 500ms，或改为事件驱动 I/O |
| **P1-7** | `WINDOW_UPDATE` 阈值 32KB 过于保守 | `grpc_client.cpp:488` | 每 32KB 发送一次 WINDOW_UPDATE | 提升至 256KB，减少控制帧 |
| **P1-8** | `InvalidateRect(NULL)` 每帧全区域重绘 | `main.cpp:777` | 整窗口 invalidate | 只在帧变化时 invalidate |
| **P1-9** | 16ms 定时器与实际帧率不匹配 | `main.cpp:1323` | 62.5Hz 轮询 24/30fps 流 | 使用 `frame_event_` 事件驱动 |

### 🟢 P2 — 可优化

| # | 问题 | 文件:行 | 方案 |
|---|------|---------|------|
| P2-1 | `get_local_so_md5` spawn `CertUtil` 进程 | `screen_capture.cpp:43-107` | 改用 Win32 `BCryptHash` 内联 MD5 |
| P2-2 | `kMaxH264QueueSize=5` 过于保守 | `screen_capture.h:86` | 改为 10-15，吸收瞬时解码慢 |
| P2-3 | `task_cv_` 100ms 超时增加输入延迟 | `input_injector.cpp:577-591` | 改用 `wait(lock, predicate)` 无超时 |
| P2-4 | `decode_loop` 100ms timeout 多余 | `screen_capture.cpp:715` | 改用 `wait()` 无超时，纯 event 驱动 |
| P2-5 | `grpc_stream_loop` 每 100ms 空转回检查 | `screen_capture.cpp:699-701` | 改用 `WaitForSingleObject` 阻塞等待 |
| P2-6 | 每次日志写 `FlushFileBuffers` | `log.h:31` | 去掉 flush，改为定时刷新 |
| P2-7 | 手写命令行解析，字符串分配多 | `main.cpp:1273-1284` | 改用 Win32 `CommandLineToArgvW` |
| P2-8 | `GetModuleFileNameA` 在多个文件中重复调用 | 多处 | WinMain 中缓存一次 |
| P2-9 | `bitrate_label` 非线程安全 static 缓冲 | `main.cpp:182-194` | 返回 `std::string` |
| P2-10 | `reconnect_loop` 2s Sleep 无中断 | `input_injector.cpp:528-575` | 改用 `condition_variable` |

---

## Phase 2 实施顺序

1. **P0-1** 编译优化 — 一行改动，全局 20-50% 提升 ← **最高 ROI**
2. **P0-2** HALFTONE → COLORONCOLOR — 渲染耗时减半
3. **P0-3** 持久 hdc shell — 消除进程创建，snapshot 和启动速度翻倍
4. **P1-1/2** gRPC 热路径 vector 消除 — 30 次/秒堆分配归零
5. **P1-4** H264 队列 move 语义 — 减少内存带宽
6. **P1-5** socket mutex 消除 — 高频触摸延迟降低
7. **P1-6/7** gRPC 流控优化 — 控制帧减少
8. **其余 P2** — 低优先增量改进
