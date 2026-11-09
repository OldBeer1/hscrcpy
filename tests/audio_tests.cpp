#include "audio_protocol.h"
#include "audio_receiver.h"
#include "audio_forwarder.h"
#include "cli.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

bool g_verbose = false;
using namespace hscrcpy;
using namespace std::chrono_literals;
namespace {
const std::string token(32, 'a');
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class F> bool until(F predicate, int ms = 6000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}
class Client {
public:
    explicit Client(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(s != INVALID_SOCKET, "test socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        require(connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "loopback connect");
    }
    ~Client() { close(); }
    void close() { if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; } }
    void write(const void *data, size_t bytes, size_t fragment = 17) {
        auto *p = static_cast<const char *>(data);
        while (bytes) {
            int n = send(s, p, static_cast<int>((std::min)(bytes, fragment)), 0);
            require(n > 0, "test socket write");
            p += n; bytes -= n;
        }
    }
    void header(const std::string &t = token) { auto h = audio::make_header(t); write(h.data(), h.size(), 3); }
    void length(uint32_t bytes) { uint8_t size[4]; audio::write_u32(size, bytes); write(size, sizeof(size), 1); }
    SOCKET s = INVALID_SOCKET;
};
std::string read_file(const std::string &path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
void protocol() {
    require(audio::valid_token(token), "valid token");
    require(!audio::valid_token("short"), "short token");
    require(!audio::valid_token(std::string(32, 'z')), "invalid token characters");
    auto h = audio::make_header(token);
    require(audio::valid_header(h.data(), token), "valid handshake");
    require(!audio::valid_header(h.data(), std::string(32, 'b')), "wrong session");
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16)}) {
        auto bad = h; bad[offset] ^= 1;
        require(!audio::valid_header(bad.data(), token), "bad format accepted");
    }
    require(audio::valid_frame(3840) && audio::valid_frame(audio::kMaxFrameBytes), "valid PCM sizes");
    require(!audio::valid_frame(0) && !audio::valid_frame(3) &&
            !audio::valid_frame(audio::kMaxFrameBytes + 4) && !audio::valid_frame(UINT32_MAX), "invalid PCM sizes");
    CliArgs args;
    char prog[] = "hscrcpy", on[] = "--audio", off[] = "--no-audio";
    char *argv[] = {prog, on, off};
    require(CliParser::parse(args, 3, argv) && args.audio_override == 0, "audio CLI precedence");
    puts("PASS protocol validation and CLI audio options");
}
void playback_and_bad_peer() {
    AudioReceiver r, other;
    require(r.start(token, {}), "receiver start");
    require(other.start(std::string(32, 'b'), {}), "second receiver start");
    require(r.port() != other.port(), "sessions must use different ports");
    { Client wrong(r.port()); wrong.header(std::string(32, 'b')); }
    Client valid(r.port()); valid.header();
    valid.length(0);
    std::vector<uint8_t> silence(3840);
    for (int i = 0; i < 25; ++i) {
        valid.length(static_cast<uint32_t>(silence.size()));
        valid.write(silence.data(), silence.size(), 137);
    }
    require(until([&] { return r.frames_received() == 25; }), "fragmented PCM must reach Windows playback");
    require(r.bytes_received() == silence.size() * 25, "received PCM byte count");
    auto start = std::chrono::steady_clock::now();
    r.stop(); other.stop();
    require(std::chrono::steady_clock::now() - start < 1s, "receiver stop must be bounded");
    puts("PASS Windows silence playback, fragmented stream, heartbeat, wrong-session rejection and parallel listeners");
}
void invalid_packets() {
    for (uint32_t size : {uint32_t(3), audio::kMaxFrameBytes + 4, UINT32_MAX}) {
        AudioReceiver r;
        std::atomic<bool> error{false};
        require(r.start(token, [&](AudioStatus s) { if (s == AudioStatus::Error) error = true; }), "receiver start");
        Client c(r.port()); c.header(); c.length(size);
        require(until([&] { return !r.running(); }), "malformed packet must disconnect");
        require(error && r.frames_received() == 0, "invalid length must not reach playback");
    }
    AudioReceiver truncated;
    require(truncated.start(token, {}), "truncation receiver start");
    { Client c(truncated.port()); c.header(); c.length(3840); uint8_t part[8]{}; c.write(part, sizeof(part)); }
    require(until([&] { return !truncated.running(); }), "truncated PCM must disconnect");
    require(truncated.frames_received() == 0, "partial PCM must not play");
    AudioReceiver waiting;
    require(waiting.start(token, {}), "waiting receiver start");
    auto start = std::chrono::steady_clock::now();
    waiting.stop();
    require(std::chrono::steady_clock::now() - start < 500ms, "cancel waiting for phone must return promptly");
    puts("PASS oversized/unaligned/truncated packet rejection and cancellation before connection");
}
void forwarder_lifecycle() {
    char temp[MAX_PATH]{};
    GetTempPathA(sizeof(temp), temp);
    std::string log = std::string(temp) + "hscrcpy-audio-test-" + std::to_string(GetCurrentProcessId()) + ".log";
    SetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_LOG", log.c_str());
    std::atomic<AudioStatus> state{AudioStatus::Off};
    {
        AudioForwarder f("audio-test-device", "127.0.0.1", "8710", [&](AudioStatus s) {
            state = s; fprintf(stderr, "Audio test state: %d\n", static_cast<int>(s));
        });
        f.start(false);
        std::this_thread::sleep_for(50ms);
        require(read_file(log).empty(), "audio disabled must not call HDC");
        SetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_MISSING", "1");
        f.set_enabled(true);
        require(until([&] { return state == AudioStatus::MissingApp; }), "missing HAP status");
        SetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_MISSING", nullptr);
        f.set_enabled(false); f.set_enabled(true);
        require(until([&] { return state == AudioStatus::Waiting; }), "launch companion without a real device");
        f.set_enabled(false);
        require(until([&] { return read_file(log).find("fport rm tcp:") != std::string::npos; }),
                "stop must remove its reverse mapping");
        state = AudioStatus::Off;
        f.set_enabled(true);
        require(until([&] {
            auto text = read_file(log); auto first = text.find(" rport tcp:");
            return first != std::string::npos && text.find(" rport tcp:", first + 1) != std::string::npos;
        }), "restart must create a second reverse mapping");
        f.stop();
    }
    auto commands = read_file(log);
    require(commands.find("--ps action stop --ps session ") != std::string::npos, "stop must identify its own phone session");
    require(commands.find("fport ls") == std::string::npos && commands.find("uninstall") == std::string::npos,
            "audio must not broadly clean forwards or uninstall apps");
    SetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_LOG", nullptr);
    DeleteFileA(log.c_str()); // Only the uniquely named log created by this test.
    puts("PASS disabled/missing-app/restart/stop lifecycle with fake HDC and exact mapping cleanup");
}
}
int main() {
    try {
        protocol(); playback_and_bad_peer(); invalid_packets(); forwarder_lifecycle();
        puts("All audio tests passed.");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
