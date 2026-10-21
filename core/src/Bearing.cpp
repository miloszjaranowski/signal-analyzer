#include "sa/Bearing.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sa {

BearingFrequencies bearingFrequencies(const BearingGeometry& geometry, double shaftHz) {
    const BearingGeometry& g = geometry;
    if (g.rollingElements < 1 || !(g.ballDiameter > 0.0) || !(g.pitchDiameter > g.ballDiameter) ||
        !(g.contactAngleDeg >= 0.0 && g.contactAngleDeg < 90.0) || !(shaftHz >= 0.0)) {
        throw std::invalid_argument("Invalid bearing geometry or shaft speed");
    }

    // Classic kinematics, assuming pure rolling (no slip).
    const double ratio =
        g.ballDiameter / g.pitchDiameter * std::cos(g.contactAngleDeg * std::numbers::pi / 180.0);
    const double n = g.rollingElements;

    BearingFrequencies f;
    f.ftf = shaftHz / 2.0 * (1.0 - ratio);
    f.bpfo = n * f.ftf;
    f.bpfi = n * shaftHz / 2.0 * (1.0 + ratio);
    f.bsf = g.pitchDiameter / (2.0 * g.ballDiameter) * shaftHz * (1.0 - ratio * ratio);
    return f;
}

double frequencyOf(const BearingFrequencies& frequencies, BearingFault fault) {
    switch (fault) {
        case BearingFault::Cage: return frequencies.ftf;
        case BearingFault::OuterRace: return frequencies.bpfo;
        case BearingFault::InnerRace: return frequencies.bpfi;
        case BearingFault::RollingElement: return frequencies.bsf;
    }
    return 0.0;
}

std::string_view shortName(BearingFault fault) {
    switch (fault) {
        case BearingFault::Cage: return "FTF";
        case BearingFault::OuterRace: return "BPFO";
        case BearingFault::InnerRace: return "BPFI";
        case BearingFault::RollingElement: return "BSF";
    }
    return "?";
}

std::string_view description(BearingFault fault) {
    switch (fault) {
        case BearingFault::Cage: return "cage";
        case BearingFault::OuterRace: return "outer race";
        case BearingFault::InnerRace: return "inner race";
        case BearingFault::RollingElement: return "rolling element";
    }
    return "?";
}

std::optional<FaultMatch> matchBearingFault(const BearingFrequencies& frequencies, double peakHz,
                                            int maxHarmonic, double tolerancePercent) {
    if (!(peakHz > 0.0)) {
        return std::nullopt;
    }
    std::optional<FaultMatch> best;
    for (const BearingFault fault : kBearingFaults) {
        const double base = frequencyOf(frequencies, fault);
        if (!(base > 0.0)) {
            continue;
        }
        for (int h = 1; h <= maxHarmonic; ++h) {
            const double expected = h * base;
            const double error = (peakHz - expected) / expected * 100.0;
            if (std::abs(error) <= tolerancePercent &&
                (!best || std::abs(error) < std::abs(best->errorPercent))) {
                best = FaultMatch{fault, h, expected, error};
            }
        }
    }
    return best;
}

}  // namespace sa
