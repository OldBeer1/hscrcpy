#include "audio_forwarder.h"
#include "audio_protocol.h"
#include "log.h"
#include <bcrypt.h>
#include <array>
#include <chrono>

namespace hscrcpy {
namespace {
constexpr const char *kBundle = "com.hscrcpy.audiocapture";
std::string companion_path() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    auto slash = p.find_last_of("\\/");
    return p.substr(0, slash == std::string::npos ? 0 : slash + 1) + "server\\audio-capture.hap";
}
std::string session_token() {
    std::array<UCHAR, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (auto b : random) { token += hex[b >> 4]; token += hex[b & 15]; }
    return token;
}
}

AudioForwarder::AudioForwarder(const std::string &serial, const std::string &ip,
                               const std::string &port, AudioReceiver::Notify notify)
    : hdc_(serial, ip, port), notify_(std::move(notify)) {}

void AudioForwarder::start(bool enabled) {
    if (thread_.joinable()) return;
    stopping_ = false;
    enabled_ = enabled;
    epoch_ = 1;
    thread_ = std::thread(&AudioForwarder::run, this);
}
void AudioForwarder::set_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
        ++epoch_;
    }
    if (!enabled) receiver_.request_stop();
    wake_.notify_all();
}
void AudioForwarder::request_stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    receiver_.request_stop();
    wake_.notify_all();
}
void AudioForwarder::stop() {
    request_stop();
    if (thread_.joinable()) thread_.join();
}
bool AudioForwarder::cancelled(uint64_t epoch) const {
    return stopping_ || !enabled_ || epoch_.load() != epoch;
}
void AudioForwarder::run() {
    uint64_t attempted = 0;
    while (!stopping_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || (enabled_ && epoch_.load() != attempted); });
        }
        if (stopping_) break;
        attempted = epoch_;
        try { session(attempted); }
        catch (const std::exception &e) {
            LOGE("Audio forwarding: %s", e.what());
            receiver_.stop();
            if (notify_ && !cancelled(attempted)) notify_(AudioStatus::Error);
        }
    }
}
void AudioForwarder::session(uint64_t epoch) {
    auto publish = [&](AudioStatus s) { if (notify_ && !cancelled(epoch)) notify_(s); };
    publish(AudioStatus::Preparing);
    std::string info;
    hdc_.shell_command(std::string("bm dump -n ") + kBundle, &info, 3);
    if (cancelled(epoch)) return;
    if (info.find(kBundle) == std::string::npos || info.find("bundleName") == std::string::npos) {
        auto hap = companion_path();
        if (GetFileAttributesA(hap.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOGW("Audio companion is missing; see audio-companion/README.md");
            publish(AudioStatus::MissingApp); return;
        }
        if (!hdc_.install_hap(hap)) { publish(AudioStatus::Error); return; }
    }
    if (cancelled(epoch)) return;
    auto token = session_token();
    if (!receiver_.start(token, [this, epoch](AudioStatus s) {
            if (notify_ && !cancelled(epoch)) notify_(s);
        })) { publish(AudioStatus::Error); return; }
    uint16_t port = receiver_.port();
    bool mapped = false, launched = false;
    auto cleanup = [&] {
        receiver_.stop();
        if (launched) {
            hdc_.shell_command(std::string("aa start -a EntryAbility -b ") + kBundle +
                               " --ps action stop --ps session " + token, nullptr, 3);
        }
        if (mapped) hdc_.reverse_remove(port, port);
    };
    try {
        if (!cancelled(epoch)) mapped = hdc_.reverse_tcp(port, port);
        if (!mapped || cancelled(epoch)) { cleanup(); publish(AudioStatus::Error); return; }
        std::string out;
        publish(AudioStatus::Waiting);
        launched = hdc_.shell_command(std::string("aa start -a EntryAbility -b ") + kBundle +
            " --ps action start --pi port " + std::to_string(port) +
            " --ps session " + token, &out, 5);
        if (!launched || out.find("error:") != std::string::npos || out.find("Error") != std::string::npos) {
            cleanup(); publish(AudioStatus::Error); return;
        }
        while (!cancelled(epoch) && receiver_.running()) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(100), [&] { return cancelled(epoch); });
        }
        cleanup();
    } catch (...) { cleanup(); throw; }
}
} // namespace hscrcpy
