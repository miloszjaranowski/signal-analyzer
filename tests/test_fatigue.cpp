#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "sa/Fatigue.hpp"

namespace {

// Cycle counts aggregated by range, the form ASTM E1049 tabulates.
std::map<double, double> countsByRange(const std::vector<sa::RainflowCycle>& cycles) {
    std::map<double, double> counts;
    for (const auto& c : cycles) {
        counts[c.range] += c.count;
    }
    return counts;
}

}  // namespace

// ASTM E1049-85, rainflow counting example (Fig. 6 / Table 4).
TEST(Rainflow, MatchesAstmE1049Example) {
    const std::vector<float> history{-2, 1, -3, 5, -1, 3, -4, 4, -2};
    const auto counts = countsByRange(sa::rainflowCycles(history));
    const std::map<double, double> expected{{3, 0.5}, {4, 1.5}, {6, 0.5}, {8, 1.0}, {9, 0.5}};
    EXPECT_EQ(counts, expected);
}

TEST(Rainflow, IgnoresSamplesBetweenReversalsAndPlateaus) {
    // Same reversals as the ASTM example, with intermediate points and repeats.
    const std::vector<float> history{-2, 0, 1, 1, -3, 5, 4, -1, -1, 3, -4, 0, 4, -2};
    const std::vector<float> reversalsOnly{-2, 1, -3, 5, -1, 3, -4, 4, -2};
    EXPECT_EQ(countsByRange(sa::rainflowCycles(history)),
              countsByRange(sa::rainflowCycles(reversalsOnly)));
}

TEST(Rainflow, StreamingInChunksEqualsOfflineCounting) {
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<float> history(5000);
    for (float& v : history) {
        v = gauss(rng);
    }

    std::vector<sa::RainflowCycle> streamed;
    sa::RainflowCounter counter;
    std::uniform_int_distribution<std::size_t> chunk(1, 300);
    for (std::size_t pos = 0; pos < history.size();) {
        const std::size_t n = std::min(chunk(rng), history.size() - pos);
        counter.push(std::span(history).subspan(pos, n),
                     [&streamed](const sa::RainflowCycle& c) { streamed.push_back(c); });
        pos += n;
    }
    const auto residue = counter.residue();
    streamed.insert(streamed.end(), residue.begin(), residue.end());

    EXPECT_EQ(countsByRange(streamed), countsByRange(sa::rainflowCycles(history)));
}

TEST(Rainflow, SineGivesOneCyclePerPeriod) {
    constexpr int periods = 50;
    constexpr int samplesPerPeriod = 40;  // peaks fall exactly on samples
    std::vector<float> x(periods * samplesPerPeriod + 1);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(3.0 * std::sin(2.0 * std::numbers::pi * double(i) / samplesPerPeriod));
    }
    double full = 0.0;
    for (const auto& c : sa::rainflowCycles(x)) {
        if (c.count == 1.0) {
            EXPECT_NEAR(c.range, 6.0, 1e-5);
            full += 1.0;
        }
    }
    EXPECT_NEAR(full, periods - 1, 1.0);  // the first and last half periods stay in the residue
}

TEST(RangeHistogram, WidensWithoutLosingCounts) {
    sa::RangeHistogram h(8);
    h.add(1.0, 10.0);
    const double firstWidth = h.binWidth();
    h.add(100.0, 2.0);  // forces several doublings
    EXPECT_GT(h.binWidth(), firstWidth);
    EXPECT_DOUBLE_EQ(h.totalCycles(), 12.0);
    EXPECT_DOUBLE_EQ(h.maxRange(), 100.0);

    const auto top = static_cast<std::size_t>(100.0 / h.binWidth());
    EXPECT_DOUBLE_EQ(h.counts()[top], 2.0);
    EXPECT_DOUBLE_EQ(h.meanRange(top), 100.0);  // exact, not the bin centre
}

TEST(Miner, DamageFollowsBasquinCurve) {
    const sa::SnCurve curve{3.0, 90.0, 2.0e6};
    sa::RangeHistogram h;
    h.add(90.0, 1.0e6);  // half the cycles to failure at the reference range
    EXPECT_NEAR(sa::minerDamage(h, curve, 1.0), 0.5, 1e-9);

    // Twice the stress range: 2^3 = 8 times the damage per cycle.
    sa::RangeHistogram doubled;
    doubled.add(90.0, 1.0e6);
    EXPECT_NEAR(sa::minerDamage(doubled, curve, 2.0), 4.0, 1e-9);

    // Damage adds up over different ranges.
    h.add(180.0, 1.0e5);
    EXPECT_NEAR(sa::minerDamage(h, curve, 1.0), 0.5 + 0.4, 1e-9);
}

TEST(Miner, FormatsDurations) {
    EXPECT_EQ(sa::formatDuration(42.0), "42 s");
    EXPECT_EQ(sa::formatDuration(40'000.0), "11.1 h");
    EXPECT_EQ(sa::formatDuration(3.0 * 365.25 * 86'400.0), "3.0 years");
    EXPECT_EQ(sa::formatDuration(1e12), "> 1000 years");
    EXPECT_EQ(sa::formatDuration(INFINITY), "> 1000 years");
}
