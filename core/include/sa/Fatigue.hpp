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

// Cycle counts over stress range with a fixed number of equal-width bins that
// widen (by merging pairs) as larger ranges arrive, so it adapts to any signal
// scale. Each bin also keeps the sum of its ranges, so damage can use the exact
// mean range per bin instead of the bin centre.
class RangeHistogram {
public:
    explicit RangeHistogram(std::size_t bins = 128);

    void add(double range, double count);
    void clear();

    double binWidth() const { return binWidth_; }  // bin i covers [i, i+1) * binWidth
    const std::vector<double>& counts() const { return counts_; }
    double meanRange(std::size_t bin) const;
    double totalCycles() const;
    double maxRange() const { return maxRange_; }

private:
    std::vector<double> counts_;
    std::vector<double> rangeSums_;  // sum of range * count per bin
    double binWidth_ = 0.0;          // 0 until the first non-zero range
    double maxRange_ = 0.0;
};

// Basquin S-N curve: a stress range S survives N = refCycles * (refRange / S)^slope cycles.
// Defaults: slope 3 with 2e6 cycles at the reference range, the usual form of
// detail categories for welded steel (Eurocode 3, IIW); refRange is the detail
// category in the same unit as the stress ranges.
struct SnCurve {
    double slope = 3.0;
    double refRange = 90.0;
    double refCycles = 2.0e6;
};

// Damage n / N of `count` cycles at one stress range (0 for a non-positive range).
double cycleDamage(double stressRange, double count, const SnCurve& curve);

// Palmgren-Miner damage D = sum n_i / N_i. Signal ranges are converted to
// stress with `stressPerUnit`; D = 1 is the predicted end of life. No endurance
// limit: every cycle contributes (conservative).
double minerDamage(const RangeHistogram& histogram, const SnCurve& curve, double stressPerUnit);

// "42 s", "3.5 min", "11.1 h", "12 days", "3.4 years", "> 1000 years".
std::string formatDuration(double seconds);

}  // namespace sa
