#ifndef HSCRCPY_CLI_H
#define HSCRCPY_CLI_H

#include <cstdint>
#include <string>
#include "protocol.h"

namespace hscrcpy {

struct CliArgs {
    std::string serial;
    uint32_t fps     = kDefaultFps;
    bool no_control  = false;
    bool show_help   = false;
    bool verbose     = false;
    int display_id   = 0;
};

class CliParser {
public:
    static bool parse(CliArgs &args, int argc, char *argv[]);
    static void print_usage(const char *prog);
};

} // namespace hscrcpy

#endif
