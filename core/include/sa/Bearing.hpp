#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace sa {

// Rolling-element bearing geometry. Diameters in any one unit (e.g. mm).
// Defaults: SKF 6205, the drive-end bearing of the CWRU bearing dataset.
struct BearingGeometry {
    int rollingElements = 9;
    double ballDiameter = 7.94;
    double pitchDiameter = 39.04;
    double contactAngleDeg = 0.0;
};

// Characteristic defect frequencies in Hz, for a rotating inner and stationary outer race.
struct BearingFrequencies {
    double ftf = 0.0;   // fundamental train (cage) frequency
    double bpfo = 0.0;  // ball pass frequency, outer race
    double bpfi = 0.0;  // ball pass frequency, inner race
    double bsf = 0.0;   // ball spin frequency; a ball defect hits a race at 2 x BSF
};

// Throws std::invalid_argument unless rollingElements >= 1, 0 < ballDiameter <
// pitchDiameter, 0 <= contactAngleDeg < 90 and shaftHz >= 0.
BearingFrequencies bearingFrequencies(const BearingGeometry& geometry, double shaftHz);

enum class BearingFault { Cage, OuterRace, InnerRace, RollingElement };

inline constexpr std::array<BearingFault, 4> kBearingFaults{
    BearingFault::Cage, BearingFault::OuterRace, BearingFault::InnerRace,
    BearingFault::RollingElement};

double frequencyOf(const BearingFrequencies& frequencies, BearingFault fault);
std::string_view shortName(BearingFault fault);    // "FTF", "BPFO", "BPFI", "BSF"
std::string_view description(BearingFault fault);  // "cage", "outer race", ...

struct FaultMatch {
    BearingFault fault = BearingFault::OuterRace;
    int harmonic = 1;           // 1 = fundamental
    double expectedHz = 0.0;
    double errorPercent = 0.0;  // (measured - expected) / expected * 100
};

// The defect-frequency harmonic (1..maxHarmonic) closest to `peakHz`, if it is
// within tolerancePercent. Real bearings slip, so measured lines sit ~1-2 % off.
std::optional<FaultMatch> matchBearingFault(const BearingFrequencies& frequencies, double peakHz,
                                            int maxHarmonic = 5, double tolerancePercent = 3.0);

}  // namespace sa
