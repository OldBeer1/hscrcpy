#include "cli.h"
#include "log.h"
#include "version.h"
#include <cstdio>
#include <cstring>

namespace hscrcpy {

bool CliParser::parse(CliArgs &args, int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-s" || arg == "--serial") && i + 1 < argc) {
            args.serial = argv[++i];
        } else if ((arg == "-f" || arg == "--fps") && i + 1 < argc) {
            args.fps = static_cast<uint32_t>(std::atoi(argv[++i]));
            if (args.fps < 1) args.fps = 1;
            if (args.fps > 60) args.fps = 60;
        } else if ((arg == "-d" || arg == "--display") && i + 1 < argc) {
            args.display_id = std::atoi(argv[++i]);
        } else if (arg == "-n" || arg == "--no-control") {
            args.no_control = true;
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--help") {
            args.show_help = true;
            return true;
        } else {
            LOGE("Unknown option: %s", arg.c_str());
            return false;
        }
    }
    return true;
}

void CliParser::print_usage(const char *prog) {
    printf("hscrcpy v" HSCRCPY_VERSION " - HarmonyOS Screen Copy\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -s, --serial=SERIAL    Device serial number\n");
    printf("  -f, --fps=FPS          Capture frame rate 1-60 (default %u)\n", kDefaultFps);
    printf("  -d, --display=ID       Display ID (default 0)\n");
    printf("  -n, --no-control       Disable device control\n");
    printf("  -v, --verbose          Verbose output\n");
    printf("      --help             Show this help\n");
    printf("\nVideo stream: gRPC H264 (primary), snapshot_display (fallback)\n");
    printf("Input injection: uitest socket JSON protocol\n");
}

} // namespace hscrcpy
