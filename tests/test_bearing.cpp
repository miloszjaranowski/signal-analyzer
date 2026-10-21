#include <gtest/gtest.h>

#include <stdexcept>

#include "sa/Bearing.hpp"

// Published multiples of shaft speed for the SKF 6205 in the CWRU bearing dataset.
TEST(Bearing, MatchesCwruReferenceFor6205) {
    const auto f = sa::bearingFrequencies(sa::BearingGeometry{}, 1.0);
    EXPECT_NEAR(f.ftf, 0.3983, 1e-3);
    EXPECT_NEAR(f.bpfo, 3.5848, 1e-3);
    EXPECT_NEAR(f.bpfi, 5.4152, 1e-3);
    EXPECT_NEAR(2.0 * f.bsf, 4.7135, 1e-3);  // CWRU lists the ball defect frequency
}

TEST(Bearing, ScalesWithShaftSpeedAndContactAngle) {
    const sa::BearingGeometry g;
    const auto at1 = sa::bearingFrequencies(g, 1.0);
    const auto at50 = sa::bearingFrequencies(g, 50.0);
    EXPECT_NEAR(at50.bpfo, 50.0 * at1.bpfo, 1e-9);
    EXPECT_NEAR(at50.bpfo + at50.bpfi, 9 * 50.0, 1e-9);  // BPFO + BPFI = n * shaft speed

    sa::BearingGeometry angular = g;
    angular.contactAngleDeg = 60.0;  // cos 60 = 0.5 halves the effective d/D
    const auto a = sa::bearingFrequencies(angular, 1.0);
    EXPECT_NEAR(a.ftf, 0.5 * (1.0 - 0.5 * g.ballDiameter / g.pitchDiameter), 1e-9);
}

TEST(Bearing, RejectsImpossibleGeometry) {
    sa::BearingGeometry g;
    g.ballDiameter = 40.0;  // larger than the pitch diameter
    EXPECT_THROW(sa::bearingFrequencies(g, 50.0), std::invalid_argument);
    EXPECT_THROW(sa::bearingFrequencies(sa::BearingGeometry{}, -1.0), std::invalid_argument);
    sa::BearingGeometry none;
    none.rollingElements = 0;
    EXPECT_THROW(sa::bearingFrequencies(none, 50.0), std::invalid_argument);
}

TEST(Bearing, MatchesPeakToFaultHarmonic) {
    const auto f = sa::bearingFrequencies(sa::BearingGeometry{}, 50.0);

    const auto outer = sa::matchBearingFault(f, f.bpfo * 1.01);  // 1 % slip
    ASSERT_TRUE(outer);
    EXPECT_EQ(outer->fault, sa::BearingFault::OuterRace);
    EXPECT_EQ(outer->harmonic, 1);
    EXPECT_NEAR(outer->errorPercent, 1.0, 1e-9);

    const auto inner = sa::matchBearingFault(f, 2.0 * f.bpfi);
    ASSERT_TRUE(inner);
    EXPECT_EQ(inner->fault, sa::BearingFault::InnerRace);
    EXPECT_EQ(inner->harmonic, 2);

    // 150 Hz: nearest are BSF 118 Hz and BPFO 179 Hz, far outside the tolerance.
    EXPECT_FALSE(sa::matchBearingFault(f, 150.0));
    EXPECT_FALSE(sa::matchBearingFault(f, 0.0));
}
