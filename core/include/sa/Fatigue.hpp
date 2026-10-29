#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace sa {

// One rainflow cycle. Ranges and means are in signal units.
struct RainflowCycle {
    double range = 0.0;  // peak to valley
    double mean = 0.0;
    double count = 1.0;  // 1 = full cycle, 0.5 = half cycle (residue)
};

// Streaming rainflow counter (ASTM E1049-85, four-point method). Samples can
// arrive in chunks of any size; the result equals counting the concatenated
// history. Only reversals are stored, and closed cycles leave the stack, so
// memory stays bounded by the residue.
class RainflowCounter {
public:
    using CycleSink = std::function<void(const RainflowCycle&)>;

    // Calls onCycle for every full cycle closed by these samples.
    void push(std::span<const float> samples, const CycleSink& onCycle);

    // What is left open so far, as half cycles: ASTM counts the residue this way
    // at the end of a history.
    std::vector<RainflowCycle> residue() const;

    void reset();

private:
    void addReversal(double value, const CycleSink& onCycle);

    std::vector<double> stack_;  // reversals not yet part of a closed cycle
    double pending_ = 0.0;       // running extreme since the last reversal
    int direction_ = 0;          // +1 rising, -1 falling, 0 before the first move
};

// Offline counting of a whole history: full cycles followed by the residue.
std::vector<RainflowCycle> rainflowCycles(std::span<const float> history);

// Rainflow matrix: cycle counts over range x mean, with a fixed number of
// equal-width bins on each axis that widen (by merging pairs) as larger values
// arrive, so it adapts to any signal scale. The mean axis is symmetric around 0.
// Each cell also keeps the sums of its ranges and means, so damage uses the exact
// averages per cell instead of bin centres.
class RainflowMatrix {
public:
    struct Cell {
        double count = 0.0;
        double rangeSum = 0.0;  // sum of range * count
        double meanSum = 0.0;   // sum of mean * count
    };

    explicit RainflowMatrix(std::size_t rangeBins = 64, std::size_t meanBins = 32);

    void add(double range, double mean, double count);
    void clear();

    std::size_t rangeBins() const { return rangeBins_; }
    std::size_t meanBins() const { return meanBins_; }
    // Range bin r covers [r, r+1) * rangeBinWidth; mean bin m covers
    // [m - meanBins/2, m - meanBins/2 + 1) * meanBinWidth.
    double rangeBinWidth() const { return rangeWidth_; }
    double meanBinWidth() const { return meanWidth_; }
    const Cell& cell(std::size_t rangeBin, std::size_t meanBin) const {
        return cells_[rangeBin * meanBins_ + meanBin];
    }

    double totalCycles() const;
    double maxRange() const { return maxRange_; }

private:
    void widenRange();
    void widenMean();
    Cell& at(std::size_t rangeBin, std::size_t meanBin) { return cells_[rangeBin * meanBins_ + meanBin]; }

    std::size_t rangeBins_;
    std::size_t meanBins_;
    std::vector<Cell> cells_;  // row-major: range bin, then mean bin
    double rangeWidth_ = 0.0;  // 0 until the first non-zero range
    double meanWidth_ = 0.0;   // 0 until the first non-zero mean
    double maxRange_ = 0.0;
};

// S-N curve: how many cycles of a stress range S a detail survives.
//   SingleSlope:    N = refCycles * (refRange / S)^slope, no limit (most conservative).
//   EnduranceLimit: the same, but ranges below the knee (N = kneeCycles) do no damage.
//   TwoSlope:       Eurocode 3 / IIW form: slope m down to the knee, slope2 from the
//                   knee to cutoffCycles, and no damage below that cut-off range.
// Defaults: detail category 90 MPa (refRange at 2e6 cycles), m = 3, knee at 5e6
// cycles, m2 = 5, cut-off at 1e8 cycles, as in EN 1993-1-9 for welded steel.
struct SnCurve {
    enum class Shape { SingleSlope, EnduranceLimit, TwoSlope };

    Shape shape = Shape::TwoSlope;
    double slope = 3.0;
    double refRange = 90.0;
    double refCycles = 2.0e6;
    double kneeCycles = 5.0e6;
    double slope2 = 5.0;
    double cutoffCycles = 1.0e8;
};

double kneeRange(const SnCurve& curve);    // stress range at kneeCycles (constant amplitude limit)
double cutoffRange(const SnCurve& curve);  // TwoSlope: no damage below; otherwise the knee range
                                           // (EnduranceLimit) or 0 (SingleSlope)

// Cycles to failure at stress range S; +infinity when S does no damage.
double cyclesToFailure(double stressRange, const SnCurve& curve);

// Mean-stress correction: converts a cycle with tensile mean stress into the
// zero-mean range that does the same damage. Compressive means are left
// uncorrected (conservative). None is the Eurocode 3 / IIW practice for welded
// joints, where residual stresses dominate the mean anyway.
struct MeanStressCorrection {
    enum class Method { None, Goodman, Gerber };

    Method method = Method::None;
    double ultimateStrength = 490.0;  // R_m, same unit as the stresses (S355 steel: 470-630 MPa)
};

// Goodman: S / (1 - Sm/Rm), Gerber: S / (1 - (Sm/Rm)^2). Sm/Rm is capped at 0.99.
double equivalentRange(double stressRange, double meanStress, const MeanStressCorrection& correction);

// Damage n / N of `count` cycles at one (already corrected) stress range.
double cycleDamage(double stressRange, double count, const SnCurve& curve);

// Palmgren-Miner damage per range bin of the matrix (D = the sum). Signal values
// are converted to stress with `stressPerUnit`; D = 1 is the predicted end of life.
std::vector<double> damageByRange(const RainflowMatrix& matrix, const SnCurve& curve,
                                  const MeanStressCorrection& correction, double stressPerUnit);
double minerDamage(const RainflowMatrix& matrix, const SnCurve& curve,
                   const MeanStressCorrection& correction, double stressPerUnit);

// "42 s", "3.5 min", "11.1 h", "12 days", "3.4 years", "> 1000 years".
std::string formatDuration(double seconds);

}  // namespace sa
