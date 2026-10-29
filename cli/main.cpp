// Headless front-end: runs the same pipeline as the GUI and prints live metrics.
// Useful on servers, in CI and for quick checks without Qt.
//
//   sa_cli [--port 5005] [--fft 4096] [--env-band 1000:4000]
//          [--rpm 3000 [--bearing 9:7.94:39.04:0]] [--stress-scale 1]
//          [--sn 3:90:2e6:5e6:5:1e8] [--mean-stress none] [--seconds 0]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sa/Bearing.hpp"
#include "sa/Dsp.hpp"
#include "sa/Pipeline.hpp"

namespace {

void printUsage() {
    std::cout << "Usage: sa_cli [--port N] [--fft N] [--env-band LO:HI] [--rpm R [--bearing N:d:D[:angle]]]\n"
                 "              [--stress-scale K] [--sn m:RANGE:CYCLES[:KNEE[:m2:CUTOFF]]]\n"
                 "              [--mean-stress none|goodman:RM|gerber:RM] [--seconds N]\n"
                 "  --port      UDP port to listen on (default 5005)\n"
                 "  --fft       FFT size, power of two (default 4096)\n"
                 "  --env-band  band-pass for envelope analysis in Hz (default 1000:4000)\n"
                 "  --rpm       shaft speed; enables matching envelope lines to bearing defects\n"
                 "  --bearing   rolling elements : ball diameter : pitch diameter [: contact angle deg]\n"
                 "              (default 9:7.94:39.04:0, SKF 6205)\n"
                 "  --stress-scale  stress per signal unit for fatigue, e.g. MPa/g (default 1)\n"
                 "  --sn        S-N curve: slope : stress range : cycles at that range, then\n"
                 "              optionally the knee cycles (endurance limit below it), then the\n"
                 "              slope below the knee and the cut-off cycles (two slopes).\n"
                 "              (default 3:90:2e6:5e6:5:1e8, EN 1993-1-9 detail category 90)\n"
                 "  --mean-stress  mean-stress correction with ultimate strength RM\n"
                 "              (default none, as for welded joints)\n"
                 "  --seconds   stop after N seconds, 0 = run until Ctrl+C (default 0)\n";
}

// "1:2.5:3" -> {1, 2.5, 3}. Throws std::invalid_argument on a malformed number.
std::vector<double> splitNumbers(const std::string& text) {
    std::vector<double> values;
    std::istringstream in(text);
    for (std::string part; std::getline(in, part, ':');) {
        values.push_back(std::stod(part));
    }
    return values;
}

// 3 values: single slope, 4: endurance limit at the knee, 6: two slopes with cut-off.
sa::SnCurve parseSnCurve(const std::string& text) {
    const auto v = splitNumbers(text);
    sa::SnCurve c;
    if (v.size() != 3 && v.size() != 4 && v.size() != 6) {
        throw std::invalid_argument("--sn expects m:RANGE:CYCLES[:KNEE[:m2:CUTOFF]]");
    }
    c.slope = v[0];
    c.refRange = v[1];
    c.refCycles = v[2];
    c.shape = sa::SnCurve::Shape::SingleSlope;
    if (v.size() >= 4) {
        c.shape = sa::SnCurve::Shape::EnduranceLimit;
        c.kneeCycles = v[3];
    }
    if (v.size() == 6) {
        c.shape = sa::SnCurve::Shape::TwoSlope;
        c.slope2 = v[4];
        c.cutoffCycles = v[5];
    }
    return c;
}

sa::MeanStressCorrection parseMeanStress(const std::string& text) {
    using Method = sa::MeanStressCorrection::Method;
    sa::MeanStressCorrection c;
    const auto colon = text.find(':');
    const std::string name = text.substr(0, colon);
    if (name == "none" && colon == std::string::npos) {
        return c;
    }
    if ((name != "goodman" && name != "gerber") || colon == std::string::npos) {
        throw std::invalid_argument("--mean-stress expects none, goodman:RM or gerber:RM");
    }
    c.method = name == "goodman" ? Method::Goodman : Method::Gerber;
    c.ultimateStrength = std::stod(text.substr(colon + 1));
    return c;
}

std::string describeMatch(const sa::AnalysisFrame& frame, const sa::BearingFrequencies& bearing) {
    if (frame.envelopePeakProminenceDb < sa::kClearLineDb) {
        return "match=-";
    }
    const auto match = sa::matchBearingFault(bearing, frame.envelopePeakHz);
    if (!match) {
        return "match=none";
    }
    char text[64];
    std::snprintf(text, sizeof text, "match=%s %dx (%+.1f%%)",
                  std::string(sa::shortName(match->fault)).c_str(), match->harmonic,
                  match->errorPercent);
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    sa::PipelineConfig config;
    sa::BearingGeometry geometry;
    std::optional<double> rpm;
    sa::SnCurve snCurve;
    sa::MeanStressCorrection meanStress;
    double stressScale = 1.0;
    int seconds = 0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (arg == "--port" && hasValue) {
                config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
            } else if (arg == "--fft" && hasValue) {
                config.fftSize = static_cast<std::size_t>(std::stoul(argv[++i]));
                config.hopSize = config.fftSize / 4;
            } else if (arg == "--env-band" && hasValue) {
                const auto band = splitNumbers(argv[++i]);
                if (band.size() != 2) {
                    throw std::invalid_argument("--env-band expects LO:HI");
                }
                config.envelopeBandLoHz = static_cast<float>(band[0]);
                config.envelopeBandHiHz = static_cast<float>(band[1]);
            } else if (arg == "--rpm" && hasValue) {
                rpm = std::stod(argv[++i]);
            } else if (arg == "--bearing" && hasValue) {
                const auto values = splitNumbers(argv[++i]);
                if (values.size() != 3 && values.size() != 4) {
                    throw std::invalid_argument("--bearing expects N:d:D[:angle]");
                }
                geometry.rollingElements = static_cast<int>(values[0]);
                geometry.ballDiameter = values[1];
                geometry.pitchDiameter = values[2];
                geometry.contactAngleDeg = values.size() == 4 ? values[3] : 0.0;
            } else if (arg == "--stress-scale" && hasValue) {
                stressScale = std::stod(argv[++i]);
            } else if (arg == "--sn" && hasValue) {
                snCurve = parseSnCurve(argv[++i]);
            } else if (arg == "--mean-stress" && hasValue) {
                meanStress = parseMeanStress(argv[++i]);
            } else if (arg == "--seconds" && hasValue) {
                seconds = std::stoi(argv[++i]);
            } else {
                printUsage();
                return arg == "--help" || arg == "-h" ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid argument: " << e.what() << '\n';
        printUsage();
        return EXIT_FAILURE;
    }

    try {
        std::optional<sa::BearingFrequencies> bearing;
        if (rpm) {
            bearing = sa::bearingFrequencies(geometry, *rpm / 60.0);
            std::printf("Bearing at %.0f rpm:", *rpm);
            for (const sa::BearingFault fault : sa::kBearingFaults) {
                std::printf("  %s=%.2f Hz", std::string(sa::shortName(fault)).c_str(),
                            sa::frequencyOf(*bearing, fault));
            }
            std::printf("\n");
        }

        sa::Pipeline pipeline(config);
        pipeline.start();
        std::cout << "Listening on UDP port " << pipeline.port() << ", FFT size "
                  << config.fftSize << ". Waiting for data...\n";

        const auto startTime = std::chrono::steady_clock::now();
        while (pipeline.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const auto stats = pipeline.stats();
            if (const auto frame = pipeline.latestFrame()) {
                const std::string match = bearing ? describeMatch(*frame, *bearing) + "  " : "";
                const double damage =
                    sa::minerDamage(frame->rainflow, snCurve, meanStress, stressScale);
                const bool belowLimit =
                    frame->rainflow.totalCycles() > 0.0 && sa::cutoffRange(snCurve) > 0.0;
                const std::string life = damage > 0.0 ? sa::formatDuration(frame->fatigueSeconds / damage)
                                         : belowLimit ? "unlimited"
                                                      : "-";
                std::printf(
                    "fs=%u Hz  RMS=%.4f  peak=%.4f  crest=%.2f  f_dom=%.2f Hz  f_env=%.2f Hz (%+.0f dB)  "
                    "%sD=%.3g life=%s  | pkts=%llu lost=%llu bad=%llu dropped=%llu buf=%zu\n",
                    frame->sampleRate, frame->rms, frame->peak,
                    frame->rms > 0 ? frame->peak / frame->rms : 0.0f, frame->dominantHz,
                    frame->envelopePeakHz, frame->envelopePeakProminenceDb, match.c_str(), damage,
                    life.c_str(),
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
