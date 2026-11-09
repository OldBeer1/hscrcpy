// SPDX-License-Identifier: Apache-2.0
#include <napi/native_api.h>
#include <multimedia/player_framework/native_avscreen_capture.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include "audio_protocol.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace hscrcpy;
class Capture {
public:
    ~Capture() { stop(); }
    bool start(uint16_t port, const std::string &token) {
        stop();
        if (!port || !audio::valid_token(token)) { set_status("error: invalid session"); return false; }
        capture_ = OH_AVScreenCapture_Create();
        if (!capture_) { set_status("error: cannot create capture"); return false; }
        OH_AVScreenCaptureConfig config{};
        config.captureMode = OH_CAPTURE_HOME_SCREEN;
        config.dataType = OH_ORIGINAL_STREAM;
        config.audioInfo.innerCapInfo.audioSampleRate = audio::kSampleRate;
        config.audioInfo.innerCapInfo.audioChannels = audio::kChannels;
        config.audioInfo.innerCapInfo.audioSource = OH_ALL_PLAYBACK;
        // Zero microphone and video parameters: capture only internal PCM.
        if (OH_AVScreenCapture_Init(capture_, config) != AV_SCREEN_CAPTURE_ERR_OK ||
            OH_AVScreenCapture_SetMicrophoneEnabled(capture_, false) != AV_SCREEN_CAPTURE_ERR_OK ||
            OH_AVScreenCapture_SetDataCallback(capture_, on_data, this) != AV_SCREEN_CAPTURE_ERR_OK ||
            OH_AVScreenCapture_SetStateCallback(capture_, on_state, this) != AV_SCREEN_CAPTURE_ERR_OK ||
            OH_AVScreenCapture_SetErrorCallback(capture_, on_error, this) != AV_SCREEN_CAPTURE_ERR_OK) {
            set_status("error: capture initialization failed"); stop(); return false;
        }
        running_ = true;
        set_status("waiting");
        try { sender_ = std::thread(&Capture::send_loop, this, port, token); }
        catch (...) { set_status("error: sender thread"); stop(); return false; }
        if (OH_AVScreenCapture_StartScreenCapture(capture_) != AV_SCREEN_CAPTURE_ERR_OK) {
            set_status("error: capture start failed"); stop(); return false;
        }
        return true;
    }
    void stop() {
        running_ = false;
        wake_.notify_all();
        if (capture_) {
            OH_AVScreenCapture_StopScreenCapture(capture_);
            OH_AVScreenCapture_Release(capture_);
            capture_ = nullptr;
        }
        if (sender_.joinable()) sender_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queued_bytes_ = 0;
        if (status_ == "waiting" || status_ == "capturing" || status_ == "private") status_ = "stopped";
    }
    std::string status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }
private:
    void set_status(std::string status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(status);
    }
    static void on_error(OH_AVScreenCapture *, int32_t code, void *user) {
        auto *self = static_cast<Capture *>(user);
        self->set_status("error: capture " + std::to_string(code));
        self->running_ = false;
        self->wake_.notify_all();
        // The ArkTS controller releases the instance outside SDK callbacks.
    }
    static void on_state(OH_AVScreenCapture *, OH_AVScreenCaptureStateCode state, void *user) {
        auto *self = static_cast<Capture *>(user);
        if (!self->running_ && (state == OH_SCREEN_CAPTURE_STATE_STARTED ||
            state == OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE ||
            state == OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE)) return;
        if (state == OH_SCREEN_CAPTURE_STATE_STARTED) self->set_status("capturing");
        else if (state == OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->queue_.clear(); self->queued_bytes_ = 0; self->status_ = "private";
        } else if (state == OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE) self->set_status("capturing");
        else if (state == OH_SCREEN_CAPTURE_STATE_CANCELED ||
                 state == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER ||
                 state == OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER ||
                 state == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL ||
                 state == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES) {
            self->set_status("stopped");
            self->running_ = false;
            self->wake_.notify_all();
        }
    }
    static void on_data(OH_AVScreenCapture *, OH_AVBuffer *buffer,
                        OH_AVScreenCaptureBufferType type, int64_t, void *user) {
        auto *self = static_cast<Capture *>(user);
        if (!self->running_ || type != OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_INNER || !buffer) return;
        OH_AVCodecBufferAttr attr{};
        auto *data = OH_AVBuffer_GetAddr(buffer);
        int capacity = OH_AVBuffer_GetCapacity(buffer);
        if (!data || OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK ||
            attr.offset < 0 || attr.size <= 0 || attr.offset > capacity ||
            attr.size > capacity - attr.offset || !audio::valid_frame(static_cast<uint32_t>(attr.size))) return;
        size_t bytes = static_cast<size_t>(attr.size);
        data += attr.offset;
        if (bytes > audio::kMaxQueuedBytes) { data += bytes - audio::kMaxQueuedBytes; bytes = audio::kMaxQueuedBytes; }
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (!self->running_ || self->status_ == "private") return;
        while (!self->queue_.empty() && self->queued_bytes_ + bytes > audio::kMaxQueuedBytes) {
            self->queued_bytes_ -= self->queue_.front().size();
            self->queue_.pop_front();
        }
        // Copy before the SDK invalidates the buffer. Never send on a capture callback.
        self->queue_.emplace_back(data, data + bytes);
        self->queued_bytes_ += bytes;
        self->wake_.notify_one();
    }
    int connect_local(uint16_t port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) return fd;
        if (errno != EINPROGRESS) { close(fd); return -1; }
        for (int i = 0; i < 15 && running_; ++i) {
            pollfd p{fd, POLLOUT, 0};
            int rc = poll(&p, 1, 100);
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0) continue;
            int error = 0; socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) return fd;
            break;
        }
        close(fd); return -1;
    }
    bool send_all(int fd, const void *data, size_t bytes) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto *p = static_cast<const uint8_t *>(data);
        while (bytes && running_ && std::chrono::steady_clock::now() < deadline) {
            pollfd event{fd, POLLOUT, 0};
            int ready = poll(&event, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
            if (!ready) continue;
            ssize_t n = send(fd, p, bytes, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (n <= 0) return false;
            p += n; bytes -= static_cast<size_t>(n);
        }
        return bytes == 0 && running_;
    }
    void send_loop(uint16_t port, const std::string &token) {
        auto header = audio::make_header(token);
        int attempts = 0;
        while (running_ && attempts++ < 5) {
            int fd = connect_local(port);
            if (fd < 0) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(300), [&] { return !running_; });
                continue;
            }
            if (send_all(fd, header.data(), header.size())) {
                while (running_) {
                    std::vector<uint8_t> frame;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        wake_.wait_for(lock, std::chrono::seconds(1), [&] { return !queue_.empty() || !running_; });
                        if (!running_) break;
                        if (!queue_.empty()) {
                            frame = std::move(queue_.front()); queue_.pop_front();
                            queued_bytes_ -= frame.size();
                        }
                    }
                    uint8_t length[4]{};
                    audio::write_u32(length, static_cast<uint32_t>(frame.size()));
                    if (!send_all(fd, length, sizeof(length)) ||
                        (!frame.empty() && !send_all(fd, frame.data(), frame.size()))) break;
                }
            }
            close(fd);
        }
        if (running_.exchange(false)) set_status("error: computer connection lost");
    }
    OH_AVScreenCapture *capture_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread sender_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t queued_bytes_ = 0;
    std::string status_ = "stopped";
};
Capture capture;

napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2]{}, result;
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    uint32_t port = 0;
    std::array<char, audio::kTokenSize + 1> token{};
    size_t length = 0;
    bool valid = argc == 2 && napi_get_value_uint32(env, argv[0], &port) == napi_ok &&
        napi_get_value_string_utf8(env, argv[1], nullptr, 0, &length) == napi_ok &&
        length == audio::kTokenSize &&
        napi_get_value_string_utf8(env, argv[1], token.data(), token.size(), &length) == napi_ok;
    bool ok = valid && port > 0 && port <= 65535 && length == audio::kTokenSize &&
        capture.start(static_cast<uint16_t>(port), std::string(token.data(), length));
    napi_get_boolean(env, ok, &result);
    return result;
}
napi_value Stop(napi_env env, napi_callback_info) {
    capture.stop();
    napi_value result; napi_get_undefined(env, &result); return result;
}
napi_value Status(napi_env env, napi_callback_info) {
    auto status = capture.status();
    napi_value result; napi_create_string_utf8(env, status.c_str(), status.size(), &result); return result;
}
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"status", nullptr, Status, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}
napi_module module = {1, 0, nullptr, Init, "audio_capture", nullptr, {0}};
extern "C" __attribute__((constructor)) void RegisterAudioCapture() { napi_module_register(&module); }
}
