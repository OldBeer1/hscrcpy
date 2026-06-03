#ifndef HSCRCPY_HDC_WRAPPER_H
#define HSCRCPY_HDC_WRAPPER_H

#include <string>
#include <vector>
#include <cstdint>

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

    std::vector<HdcDevice> list_devices();
    bool forward_tcp(uint16_t local_port, uint16_t remote_port);
    bool forward_localabstract(uint16_t local_port, const std::string &socket_name);
    bool forward_remove(uint16_t local_port, bool is_abstract, const std::string &socket_name = "");
    bool forward_remove_all();
    bool shell_command(const std::string &cmd, std::string *output = nullptr, int timeout_sec = 30);
    bool push_file(const std::string &local_path, const std::string &remote_path);
    bool pull_file(const std::string &remote_path, const std::string &local_path);

    const std::string &serial() const { return serial_; }
    const std::string &ip() const { return ip_; }
    const std::string &port() const { return port_; }

private:
    int run_process(const std::string &cmd, std::string *output = nullptr, int timeout_ms = 30000);
    std::string build_hdc_cmd(const std::string &subcmd);

    std::string serial_;
    std::string ip_;
    std::string port_;
};

} // namespace hscrcpy

#endif
