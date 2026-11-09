#ifndef HSCRCPY_HDC_WRAPPER_H
#define HSCRCPY_HDC_WRAPPER_H

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hscrcpy {

struct HdcDevice {
    std::string serial;
    std::string state;
    std::string ip;
    std::string port;
};

class HdcWrapper {
public:
    explicit HdcWrapper(const std::string &serial = "",
                        const std::string &ip = "127.0.0.1",
                        const std::string &port = "8710");
    ~HdcWrapper();

    bool ensure_connected();
    bool is_device_reachable();
    bool try_reconnect_new_port();

    std::vector<HdcDevice> list_devices();
    bool forward_tcp(uint16_t local_port, uint16_t remote_port);
    bool reverse_tcp(uint16_t device_port, uint16_t local_port);
    bool reverse_remove(uint16_t device_port, uint16_t local_port);
    bool install_hap(const std::string &path);
    bool forward_localabstract(uint16_t local_port, const std::string &socket_name);
    bool forward_remove(uint16_t local_port, bool is_abstract, const std::string &socket_name = "");
    bool forward_remove_all();
    int forward_count();
    bool cleanup_stale_forwards(const std::vector<uint16_t> &our_ports = {});
    void register_forward_port(uint16_t port);
    bool shell_command(const std::string &cmd, std::string *output = nullptr, int timeout_sec = 30);
    bool push_file(const std::string &local_path, const std::string &remote_path, std::string *output = nullptr);
    bool pull_file(const std::string &remote_path, const std::string &local_path, std::string *output = nullptr);

    const std::string &serial() const { return serial_; }
    const std::string &ip() const { return ip_; }
    const std::string &port() const { return port_; }
    bool is_tcp() const;

    void set_cancel(bool v) { cancel_flag_.store(v); }

    bool get_screen_resolution(uint32_t &width, uint32_t &height);

private:
    void update_serial(const std::string &new_serial);
    int run_process(const std::string &cmd, std::string *output = nullptr, int timeout_ms = 30000);
    std::string build_hdc_cmd(const std::string &subcmd);

    std::string serial_;
    std::string ip_;
    std::string port_;
    std::vector<uint16_t> our_forward_ports_;  // Track our own forward ports for safe cleanup
    std::atomic<bool> cancel_flag_{false};     // Set by ScreenCapture::request_stop() to interrupt blocking loops
};

} // namespace hscrcpy

#endif
