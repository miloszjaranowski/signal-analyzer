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

TEST(RainflowMatrix, WidensBothAxesWithoutLosingCycles) {
    sa::RainflowMatrix m(8, 4);
    m.add(1.0, 0.0, 10.0);     // zero mean before the mean axis has a width
    m.add(1.0, 0.5, 3.0);
    m.add(100.0, -40.0, 2.0);  // forces doublings on both axes
    EXPECT_DOUBLE_EQ(m.totalCycles(), 15.0);
    EXPECT_DOUBLE_EQ(m.maxRange(), 100.0);

    const auto r = static_cast<std::size_t>(100.0 / m.rangeBinWidth());
    const auto k = static_cast<std::size_t>(std::floor(-40.0 / m.meanBinWidth()) +
                                            static_cast<double>(m.meanBins() / 2));
    const auto& cell = m.cell(r, k);
    EXPECT_DOUBLE_EQ(cell.count, 2.0);
    EXPECT_DOUBLE_EQ(cell.rangeSum / cell.count, 100.0);  // exact averages, not bin centres
    EXPECT_DOUBLE_EQ(cell.meanSum / cell.count, -40.0);
}

// EN 1993-1-9: constant amplitude fatigue limit at 5e6 cycles is 0.737 x the
// detail category, the cut-off limit at 1e8 cycles 0.549 x that.
TEST(SnCurve, MatchesEurocodeTwoSlopeConstants) {
    const sa::SnCurve ec;
    const double knee = sa::kneeRange(ec);
    EXPECT_NEAR(knee / ec.refRange, 0.737, 1e-3);
    EXPECT_NEAR(sa::cutoffRange(ec) / knee, 0.549, 1e-3);

    EXPECT_NEAR(sa::cyclesToFailure(ec.refRange, ec), 2.0e6, 1e-3);
    EXPECT_NEAR(sa::cyclesToFailure(knee, ec), 5.0e6, 1.0);  // continuous at the knee
    EXPECT_NEAR(sa::cyclesToFailure(0.8 * knee, ec), 5.0e6 * std::pow(1.0 / 0.8, 5.0), 1.0);
    EXPECT_TRUE(std::isinf(sa::cyclesToFailure(0.99 * sa::cutoffRange(ec), ec)));
}

TEST(SnCurve, EnduranceLimitAndSingleSlope) {
    sa::SnCurve limit;
    limit.shape = sa::SnCurve::Shape::EnduranceLimit;
    const double knee = sa::kneeRange(limit);
    EXPECT_NEAR(sa::cyclesToFailure(knee, limit), 5.0e6, 1.0);
    EXPECT_TRUE(std::isinf(sa::cyclesToFailure(0.99 * knee, limit)));

    sa::SnCurve single;
    single.shape = sa::SnCurve::Shape::SingleSlope;
    EXPECT_NEAR(sa::cyclesToFailure(0.5 * knee, single), 8.0 * 5.0e6, 10.0);  // m = 3 all the way
}

TEST(MeanStress, GoodmanAndGerber) {
    sa::MeanStressCorrection c;
    c.ultimateStrength = 500.0;
    EXPECT_DOUBLE_EQ(sa::equivalentRange(100.0, 250.0, c), 100.0);  // None

    c.method = sa::MeanStressCorrection::Method::Goodman;
    EXPECT_DOUBLE_EQ(sa::equivalentRange(100.0, 250.0, c), 200.0);  // 100 / (1 - 0.5)
    EXPECT_DOUBLE_EQ(sa::equivalentRange(100.0, -250.0, c), 100.0);  // compressive mean

    c.method = sa::MeanStressCorrection::Method::Gerber;
    EXPECT_NEAR(sa::equivalentRange(100.0, 250.0, c), 100.0 / 0.75, 1e-9);  // 100 / (1 - 0.25)
}

TEST(Miner, DamageFollowsSnCurveAndMeanStress) {
    sa::SnCurve single;
    single.shape = sa::SnCurve::Shape::SingleSlope;
    const sa::MeanStressCorrection none;

    sa::RainflowMatrix m;
    m.add(90.0, 0.0, 1.0e6);  // half the cycles to failure at the reference range
    EXPECT_NEAR(sa::minerDamage(m, single, none, 1.0), 0.5, 1e-9);
    EXPECT_NEAR(sa::minerDamage(m, single, none, 2.0), 4.0, 1e-9);  // 2x range: 2^3 x damage
    m.add(180.0, 0.0, 1.0e5);
    EXPECT_NEAR(sa::minerDamage(m, single, none, 1.0), 0.5 + 0.4, 1e-9);

    // Two-slope curve: a billion cycles below the cut-off do no damage at all.
    sa::RainflowMatrix small;
    small.add(30.0, 0.0, 1.0e9);
    EXPECT_EQ(sa::minerDamage(small, sa::SnCurve{}, none, 1.0), 0.0);

    // Goodman with a mean of half the ultimate strength doubles the range: 8x damage.
    sa::RainflowMatrix tensile;
    tensile.add(90.0, 250.0, 1.0e5);
    const sa::MeanStressCorrection goodman{sa::MeanStressCorrection::Method::Goodman, 500.0};
    EXPECT_NEAR(sa::minerDamage(tensile, single, goodman, 1.0),
                8.0 * sa::minerDamage(tensile, single, none, 1.0), 1e-12);
}

TEST(Miner, FormatsDurations) {
    EXPECT_EQ(sa::formatDuration(42.0), "42 s");
    EXPECT_EQ(sa::formatDuration(40'000.0), "11.1 h");
    EXPECT_EQ(sa::formatDuration(3.0 * 365.25 * 86'400.0), "3.0 years");
    EXPECT_EQ(sa::formatDuration(1e12), "> 1000 years");
    EXPECT_EQ(sa::formatDuration(INFINITY), "> 1000 years");
}
