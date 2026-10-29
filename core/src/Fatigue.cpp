#include "sa/Fatigue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace sa {

void RainflowCounter::push(std::span<const float> samples, const CycleSink& onCycle) {
    for (const float sample : samples) {
        const double x = sample;
        if (stack_.empty()) {
            stack_.push_back(x);  // the start of the history is a reversal
            pending_ = x;
            continue;
        }
        if (direction_ == 0) {
            if (x != stack_.back()) {
                direction_ = x > stack_.back() ? 1 : -1;
                pending_ = x;
            }
            continue;
        }
        if ((x - pending_) * direction_ > 0.0) {
            pending_ = x;  // still moving the same way: a new extreme
        } else if (x != pending_) {
            addReversal(pending_, onCycle);  // turned around: the extreme was a reversal
            direction_ = -direction_;
            pending_ = x;
        }
    }
}

void RainflowCounter::addReversal(double value, const CycleSink& onCycle) {
    stack_.push_back(value);
    // Four-point rule: the inner range closes a cycle when it is enclosed by
    // both of its neighbours.
    while (stack_.size() >= 4) {
        const std::size_t n = stack_.size();
        const double a = stack_[n - 4];
        const double b = stack_[n - 3];
        const double c = stack_[n - 2];
        const double d = stack_[n - 1];
        const double inner = std::abs(c - b);
        if (inner > std::abs(b - a) || inner > std::abs(d - c)) {
            break;
        }
        onCycle({inner, (b + c) / 2.0, 1.0});
        stack_.erase(stack_.end() - 3, stack_.end() - 1);  // drop b and c, keep d
    }
}

std::vector<RainflowCycle> RainflowCounter::residue() const {
    std::vector<RainflowCycle> cycles;
    if (stack_.empty()) {
        return cycles;
    }
    std::vector<double> points = stack_;
    if (direction_ != 0) {
        points.push_back(pending_);  // the open extreme ends the history for now
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        cycles.push_back({std::abs(points[i] - points[i - 1]), (points[i] + points[i - 1]) / 2.0, 0.5});
    }
    return cycles;
}

void RainflowCounter::reset() {
    stack_.clear();
    pending_ = 0.0;
    direction_ = 0;
}

std::vector<RainflowCycle> rainflowCycles(std::span<const float> history) {
    std::vector<RainflowCycle> cycles;
    RainflowCounter counter;
    counter.push(history, [&cycles](const RainflowCycle& c) { cycles.push_back(c); });
    const auto residue = counter.residue();
    cycles.insert(cycles.end(), residue.begin(), residue.end());
    return cycles;
}

RainflowMatrix::RainflowMatrix(std::size_t rangeBins, std::size_t meanBins)
    : rangeBins_(std::max<std::size_t>(rangeBins, 2)),
      meanBins_(std::max<std::size_t>(meanBins, 2) & ~std::size_t{1}),  // even: symmetric around 0
      cells_(rangeBins_ * meanBins_) {}

void RainflowMatrix::add(double range, double mean, double count) {
    if (!(range > 0.0) || !(count > 0.0) || !std::isfinite(mean)) {
        return;
    }
    // The first value of each axis lands halfway out, leaving room both ways.
    if (rangeWidth_ == 0.0) {
        rangeWidth_ = range / static_cast<double>(rangeBins_ / 2);
    }
    while (range >= rangeWidth_ * static_cast<double>(rangeBins_)) {
        widenRange();
    }
    const auto half = static_cast<double>(meanBins_ / 2);
    if (meanWidth_ == 0.0 && mean != 0.0) {
        meanWidth_ = std::abs(mean) / (half / 2.0);
    }
    if (meanWidth_ > 0.0) {
        while (mean >= meanWidth_ * half || mean < -meanWidth_ * half) {
            widenMean();
        }
    }

    // Until the first non-zero mean every mean is 0, which is bin meanBins/2 at any width.
    const auto r = std::min(rangeBins_ - 1, static_cast<std::size_t>(range / rangeWidth_));
    const auto m = meanWidth_ > 0.0
                       ? std::min(meanBins_ - 1,
                                  static_cast<std::size_t>(std::floor(mean / meanWidth_) + half))
                       : meanBins_ / 2;
    Cell& cell = at(r, m);
    cell.count += count;
    cell.rangeSum += range * count;
    cell.meanSum += mean * count;
    maxRange_ = std::max(maxRange_, range);
}

void RainflowMatrix::widenRange() {
    // Double the width: new range bin r holds old bins 2r and 2r+1.
    std::vector<Cell> merged(cells_.size());
    for (std::size_t r = 0; r < rangeBins_; ++r) {
        for (std::size_t m = 0; m < meanBins_; ++m) {
            const Cell& src = at(r, m);
            Cell& dst = merged[(r / 2) * meanBins_ + m];
            dst.count += src.count;
            dst.rangeSum += src.rangeSum;
            dst.meanSum += src.meanSum;
        }
    }
    cells_.swap(merged);
    rangeWidth_ *= 2.0;
}

void RainflowMatrix::widenMean() {
    // Double the width around 0: signed bin k = m - meanBins/2 moves to floor(k / 2).
    const auto half = static_cast<std::ptrdiff_t>(meanBins_ / 2);
    std::vector<Cell> merged(cells_.size());
    for (std::size_t r = 0; r < rangeBins_; ++r) {
        for (std::size_t m = 0; m < meanBins_; ++m) {
            const std::ptrdiff_t k = static_cast<std::ptrdiff_t>(m) - half;
            const std::ptrdiff_t newK = k >= 0 ? k / 2 : -((-k + 1) / 2);
            const Cell& src = at(r, m);
            Cell& dst = merged[r * meanBins_ + static_cast<std::size_t>(newK + half)];
            dst.count += src.count;
            dst.rangeSum += src.rangeSum;
            dst.meanSum += src.meanSum;
        }
    }
    cells_.swap(merged);
    meanWidth_ *= 2.0;
}

void RainflowMatrix::clear() {
    std::fill(cells_.begin(), cells_.end(), Cell{});
    rangeWidth_ = 0.0;
    meanWidth_ = 0.0;
    maxRange_ = 0.0;
}

double RainflowMatrix::totalCycles() const {
    double total = 0.0;
    for (const Cell& c : cells_) {
        total += c.count;
    }
    return total;
}

double kneeRange(const SnCurve& curve) {
    return curve.refRange * std::pow(curve.refCycles / curve.kneeCycles, 1.0 / curve.slope);
}

double cutoffRange(const SnCurve& curve) {
    switch (curve.shape) {
        case SnCurve::Shape::SingleSlope: return 0.0;
        case SnCurve::Shape::EnduranceLimit: return kneeRange(curve);
        case SnCurve::Shape::TwoSlope:
            return kneeRange(curve) * std::pow(curve.kneeCycles / curve.cutoffCycles, 1.0 / curve.slope2);
    }
    return 0.0;
}

double cyclesToFailure(double stressRange, const SnCurve& curve) {
    constexpr double kNoDamage = std::numeric_limits<double>::infinity();
    if (!(stressRange > 0.0) || stressRange < cutoffRange(curve)) {
        return kNoDamage;
    }
    if (curve.shape == SnCurve::Shape::TwoSlope) {
        const double knee = kneeRange(curve);
        if (stressRange < knee) {
            return curve.kneeCycles * std::pow(knee / stressRange, curve.slope2);
        }
    }
    return curve.refCycles * std::pow(curve.refRange / stressRange, curve.slope);
}

double equivalentRange(double stressRange, double meanStress, const MeanStressCorrection& correction) {
    using Method = MeanStressCorrection::Method;
    if (correction.method == Method::None || !(meanStress > 0.0) ||
        !(correction.ultimateStrength > 0.0)) {
        return stressRange;
    }
    const double ratio = std::min(meanStress / correction.ultimateStrength, 0.99);
    const double factor = correction.method == Method::Goodman ? 1.0 - ratio : 1.0 - ratio * ratio;
    return stressRange / factor;
}

double cycleDamage(double stressRange, double count, const SnCurve& curve) {
    if (!(count > 0.0)) {
        return 0.0;
    }
    return count / cyclesToFailure(stressRange, curve);  // n / inf = 0 below the cut-off
}

std::vector<double> damageByRange(const RainflowMatrix& matrix, const SnCurve& curve,
                                  const MeanStressCorrection& correction, double stressPerUnit) {
    std::vector<double> damage(matrix.rangeBins());
    for (std::size_t r = 0; r < matrix.rangeBins(); ++r) {
        for (std::size_t m = 0; m < matrix.meanBins(); ++m) {
            const RainflowMatrix::Cell& c = matrix.cell(r, m);
            if (c.count > 0.0) {
                const double range = c.rangeSum / c.count * stressPerUnit;
                const double mean = c.meanSum / c.count * stressPerUnit;
                damage[r] += cycleDamage(equivalentRange(range, mean, correction), c.count, curve);
            }
        }
    }
    return damage;
}

double minerDamage(const RainflowMatrix& matrix, const SnCurve& curve,
                   const MeanStressCorrection& correction, double stressPerUnit) {
    const auto perRange = damageByRange(matrix, curve, correction, stressPerUnit);
    return std::accumulate(perRange.begin(), perRange.end(), 0.0);
}

std::string formatDuration(double seconds) {
    constexpr double kMinute = 60.0;
    constexpr double kHour = 60.0 * kMinute;
    constexpr double kDay = 24.0 * kHour;
    constexpr double kYear = 365.25 * kDay;

    char text[32];
    if (!std::isfinite(seconds) || seconds >= 1000.0 * kYear) {
        return "> 1000 years";
    }
    if (seconds < kMinute) {
        std::snprintf(text, sizeof text, "%.0f s", seconds);
    } else if (seconds < kHour) {
        std::snprintf(text, sizeof text, "%.1f min", seconds / kMinute);
    } else if (seconds < kDay) {
        std::snprintf(text, sizeof text, "%.1f h", seconds / kHour);
    } else if (seconds < kYear) {
        std::snprintf(text, sizeof text, "%.0f days", seconds / kDay);
    } else {
        std::snprintf(text, sizeof text, "%.1f years", seconds / kYear);
    }
    return text;
}

}  // namespace sa
