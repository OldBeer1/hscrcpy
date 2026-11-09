#ifndef HSCRCPY_AUDIO_FORWARDER_H
#define HSCRCPY_AUDIO_FORWARDER_H

#include "audio_receiver.h"
#include "hdc_wrapper.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hscrcpy {
class AudioForwarder {
public:
    AudioForwarder(const std::string &serial, const std::string &ip, const std::string &port,
                   AudioReceiver::Notify notify);
    ~AudioForwarder() { stop(); }
    void start(bool enabled);
    void set_enabled(bool enabled);
    void request_stop();
    void stop();
private:
    bool cancelled(uint64_t epoch) const;
    void run();
    void session(uint64_t epoch);
    HdcWrapper hdc_;
    AudioReceiver receiver_;
    AudioReceiver::Notify notify_;
    std::atomic<bool> stopping_{false}, enabled_{false};
    std::atomic<uint64_t> epoch_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};
} // namespace hscrcpy
#endif
