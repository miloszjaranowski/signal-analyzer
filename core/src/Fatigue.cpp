#include "sa/Fatigue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

RangeHistogram::RangeHistogram(std::size_t bins) : counts_(bins), rangeSums_(bins) {}

void RangeHistogram::add(double range, double count) {
    if (!(range > 0.0) || !(count > 0.0)) {
        return;
    }
    const std::size_t bins = counts_.size();
    if (binWidth_ == 0.0) {
        binWidth_ = range / static_cast<double>(bins / 2);  // first range lands mid-scale
    }
    while (range >= binWidth_ * static_cast<double>(bins)) {
        // Double the width: new bin i is old bins 2i and 2i+1, the upper half empties.
        for (std::size_t i = 0; i < bins / 2; ++i) {
            counts_[i] = counts_[2 * i] + counts_[2 * i + 1];
            rangeSums_[i] = rangeSums_[2 * i] + rangeSums_[2 * i + 1];
        }
        std::fill(counts_.begin() + static_cast<std::ptrdiff_t>(bins / 2), counts_.end(), 0.0);
        std::fill(rangeSums_.begin() + static_cast<std::ptrdiff_t>(bins / 2), rangeSums_.end(), 0.0);
        binWidth_ *= 2.0;
    }
    const auto bin = static_cast<std::size_t>(range / binWidth_);
    counts_[bin] += count;
    rangeSums_[bin] += range * count;
    maxRange_ = std::max(maxRange_, range);
}

void RangeHistogram::clear() {
    std::fill(counts_.begin(), counts_.end(), 0.0);
    std::fill(rangeSums_.begin(), rangeSums_.end(), 0.0);
    binWidth_ = 0.0;
    maxRange_ = 0.0;
}

double RangeHistogram::meanRange(std::size_t bin) const {
    return counts_[bin] > 0.0 ? rangeSums_[bin] / counts_[bin] : 0.0;
}

double RangeHistogram::totalCycles() const {
    double total = 0.0;
    for (const double c : counts_) {
        total += c;
    }
    return total;
}

double cycleDamage(double stressRange, double count, const SnCurve& curve) {
    if (!(stressRange > 0.0) || !(count > 0.0)) {
        return 0.0;
    }
    const double cyclesToFailure = curve.refCycles * std::pow(curve.refRange / stressRange, curve.slope);
    return count / cyclesToFailure;
}

double minerDamage(const RangeHistogram& histogram, const SnCurve& curve, double stressPerUnit) {
    double damage = 0.0;
    for (std::size_t i = 0; i < histogram.counts().size(); ++i) {
        damage += cycleDamage(histogram.meanRange(i) * stressPerUnit, histogram.counts()[i], curve);
    }
    return damage;
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
