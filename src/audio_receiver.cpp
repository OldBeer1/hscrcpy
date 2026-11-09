#include "audio_receiver.h"
#include "audio_protocol.h"
#include "log.h"
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

namespace hscrcpy {
namespace {
class WavePlayer {
public:
    ~WavePlayer() {
        reset();
        if (device_) {
            for (auto &s : slots_) if (s.header.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(device_, &s.header, sizeof(WAVEHDR));
            waveOutClose(device_);
        }
    }
    bool open() {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = audio::kChannels;
        f.nSamplesPerSec = audio::kSampleRate;
        f.wBitsPerSample = audio::kBits;
        f.nBlockAlign = audio::kBlockAlign;
        f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        return waveOutOpen(&device_, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }
    bool write(const uint8_t *data, size_t bytes) {
        if (bytes > audio::kMaxQueuedBytes) {
            data += bytes - audio::kMaxQueuedBytes;
            bytes = audio::kMaxQueuedBytes;
        }
        size_t pending = 0;
        Slot *free = nullptr;
        for (auto &s : slots_) {
            if (s.header.dwFlags & WHDR_INQUEUE) pending += s.header.dwBufferLength;
            else if (!free) free = &s;
        }
        // Real-time audio must not accumulate seconds of stale sound.
        if (!free || pending + bytes > audio::kMaxQueuedBytes) {
            if (!reset()) return false;
            free = &slots_[0];
        }
        if ((free->header.dwFlags & WHDR_PREPARED) &&
            waveOutUnprepareHeader(device_, &free->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        std::memcpy(free->data.data(), data, bytes);
        free->header = {};
        free->header.lpData = reinterpret_cast<LPSTR>(free->data.data());
        free->header.dwBufferLength = static_cast<DWORD>(bytes);
        if (waveOutPrepareHeader(device_, &free->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        return waveOutWrite(device_, &free->header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR;
    }
private:
    bool reset() { return !device_ || waveOutReset(device_) == MMSYSERR_NOERROR; }
    struct Slot {
        WAVEHDR header{};
        std::array<uint8_t, audio::kMaxFrameBytes> data{};
    };
    HWAVEOUT device_ = nullptr;
    std::array<Slot, 8> slots_{};
};
}

bool AudioReceiver::start(const std::string &token, Notify notify) {
    stop();
    if (!audio::valid_token(token)) return false;
    WSADATA ws{};
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) return false;
    winsock_ = true;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) { stop(); return false; }
    BOOL exclusive = TRUE;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int size = sizeof(addr);
    if (setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) != 0 ||
        bind(listener_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listener_, 2) != 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr *>(&addr), &size) != 0) {
        stop(); return false;
    }
    port_ = ntohs(addr.sin_port);
    token_ = token;
    notify_ = std::move(notify);
    frames_ = 0;
    bytes_ = 0;
    stopping_ = false;
    active_ = true;
    try { thread_ = std::thread(&AudioReceiver::run, this); }
    catch (...) { active_ = false; stop(); return false; }
    return true;
}

void AudioReceiver::stop() {
    request_stop();
    if (thread_.joinable()) thread_.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
    if (winsock_) { WSACleanup(); winsock_ = false; }
    active_ = false;
    port_ = 0;
}

bool AudioReceiver::read_exact(SOCKET s, void *data, size_t bytes, unsigned timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto *p = static_cast<char *>(data);
    while (bytes && !stopping_) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        fd_set reads; FD_ZERO(&reads); FD_SET(s, &reads);
        timeval tv{0, 100000};
        int ready = select(0, &reads, nullptr, nullptr, &tv);
        if (ready < 0) return false;
        if (!ready) continue;
        int n = recv(s, p, static_cast<int>(bytes), 0);
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<size_t>(n);
    }
    return bytes == 0 && !stopping_;
}

void AudioReceiver::run() {
    auto publish = [this](AudioStatus s) { if (notify_ && !stopping_) notify_(s); };
    SOCKET client = INVALID_SOCKET;
    try {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        bool accepted = false;
        while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
            fd_set reads; FD_ZERO(&reads); FD_SET(listener_, &reads);
            timeval tv{0, 100000};
            int ready = select(0, &reads, nullptr, nullptr, &tv);
            if (ready < 0) break;
            if (!ready) continue;
            client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            std::array<uint8_t, audio::kHeaderSize> header{};
            if (read_exact(client, header.data(), header.size(), 3000) &&
                audio::valid_header(header.data(), token_)) {
                accepted = true; break;
            }
            closesocket(client); client = INVALID_SOCKET;
        }
        if (accepted && !stopping_) {
            WavePlayer player;
            if (!player.open()) { LOGE("Audio: cannot open the Windows playback device"); publish(AudioStatus::Error); }
            else {
                std::array<uint8_t, audio::kMaxFrameBytes> pcm{};
                std::array<uint8_t, 4> length{};
                bool playing = false, failed = false;
                while (!stopping_ && read_exact(client, length.data(), length.size(), 5000)) {
                    uint32_t bytes = audio::read_u32(length.data());
                    if (bytes == 0) continue; // Heartbeat, including silence/privacy pauses.
                    if (!audio::valid_frame(bytes)) {
                        LOGE("Audio: rejected invalid PCM packet (%u bytes)", bytes);
                        failed = true; publish(AudioStatus::Error); break;
                    }
                    if (!read_exact(client, pcm.data(), bytes, 3000)) break;
                    if (!player.write(pcm.data(), bytes)) { failed = true; publish(AudioStatus::Error); break; }
                    ++frames_;
                    bytes_ += bytes;
                    if (!playing) { playing = true; publish(AudioStatus::Playing); }
                }
                if (!stopping_ && !failed) publish(AudioStatus::Disconnected);
            }
        } else if (!stopping_) publish(AudioStatus::Disconnected);
    } catch (const std::exception &e) {
        LOGE("Audio receiver: %s", e.what());
        publish(AudioStatus::Error);
    }
    if (client != INVALID_SOCKET) closesocket(client);
    active_ = false;
}
} // namespace hscrcpy
