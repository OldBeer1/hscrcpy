// Test fixture: never talks to a device.
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>
int main(int argc, char **argv) {
    std::string command;
    for (int i = 1; i < argc; ++i) { if (!command.empty()) command += ' '; command += argv[i]; }
    char log[MAX_PATH]{};
    GetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_LOG", log, sizeof(log));
    if (*log) { std::ofstream out(log, std::ios::app); out << command << '\n'; }
    if (command.find("bm dump") != std::string::npos) {
        if (GetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_MISSING", nullptr, 0)) puts("bundle not found");
        else puts("{\"bundleName\":\"com.hscrcpy.audiocapture\"}");
    } else if (command.find("aa start") != std::string::npos) puts("start ability successfully.");
    else if (command.find("echo ok") != std::string::npos) puts("ok");
    else puts("OK");
    return 0;
}
