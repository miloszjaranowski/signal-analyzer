// Headless front-end: runs the same pipeline as the GUI and prints live metrics.
// Useful on servers, in CI and for quick checks without Qt.
//
//   sa_cli [--port 5005] [--fft 4096] [--seconds 0]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "sa/Pipeline.hpp"

namespace {

void printUsage() {
    std::cout << "Usage: sa_cli [--port N] [--fft N] [--seconds N]\n"
                 "  --port     UDP port to listen on (default 5005)\n"
                 "  --fft      FFT size, power of two (default 4096)\n"
                 "  --seconds  stop after N seconds, 0 = run until Ctrl+C (default 0)\n";
}

}  // namespace

int main(int argc, char** argv) {
    sa::PipelineConfig config;
    int seconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--port" && hasValue) {
            config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--fft" && hasValue) {
            config.fftSize = static_cast<std::size_t>(std::stoul(argv[++i]));
            config.hopSize = config.fftSize / 4;
        } else if (arg == "--seconds" && hasValue) {
            seconds = std::stoi(argv[++i]);
        } else {
            printUsage();
            return arg == "--help" || arg == "-h" ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    try {
        sa::Pipeline pipeline(config);
        pipeline.start();
        std::cout << "Listening on UDP port " << pipeline.port() << ", FFT size "
                  << config.fftSize << ". Waiting for data...\n";

        const auto startTime = std::chrono::steady_clock::now();
        while (pipeline.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const auto stats = pipeline.stats();
            if (const auto frame = pipeline.latestFrame()) {
                std::printf(
                    "fs=%u Hz  RMS=%.4f  peak=%.4f  crest=%.2f  f_dom=%.2f Hz  "
                    "| pkts=%llu lost=%llu bad=%llu dropped=%llu buf=%zu\n",
                    frame->sampleRate, frame->rms, frame->peak,
                    frame->rms > 0 ? frame->peak / frame->rms : 0.0f, frame->dominantHz,
                    static_cast<unsigned long long>(stats.packetsReceived),
                    static_cast<unsigned long long>(stats.packetsLost),
                    static_cast<unsigned long long>(stats.packetsMalformed),
                    static_cast<unsigned long long>(stats.samplesDropped), stats.bufferFill);
                std::fflush(stdout);
            }

            const auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (seconds > 0 && elapsed >= std::chrono::seconds(seconds)) {
                break;
            }
        }

        pipeline.stop();
        if (const auto err = pipeline.lastError(); !err.empty()) {
            std::cerr << err << '\n';
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
