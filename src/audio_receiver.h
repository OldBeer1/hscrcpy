#ifndef HSCRCPY_AUDIO_RECEIVER_H
#define HSCRCPY_AUDIO_RECEIVER_H

#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace hscrcpy {
enum class AudioStatus { Off, Preparing, Waiting, Playing, MissingApp, Error, Disconnected };

class AudioReceiver {
public:
    using Notify = std::function<void(AudioStatus)>;
    ~AudioReceiver() { stop(); }
    bool start(const std::string &token, Notify notify);
    void request_stop() { stopping_ = true; }
    void stop();
    uint16_t port() const { return port_; }
    bool running() const { return active_; }
    uint64_t frames_received() const { return frames_; }
    uint64_t bytes_received() const { return bytes_; }
private:
    bool read_exact(SOCKET s, void *data, size_t bytes, unsigned timeout_ms);
    void run();
    std::atomic<bool> stopping_{true}, active_{false};
    std::atomic<uint64_t> frames_{0}, bytes_{0};
    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    bool winsock_ = false;
    std::string token_;
    Notify notify_;
    std::thread thread_;
};
} // namespace hscrcpy
#endif
