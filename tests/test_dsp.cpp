#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

#include "sa/Dsp.hpp"

namespace {

std::vector<float> sine(std::size_t n, double freq, double fs, double amplitude) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * std::numbers::pi * freq * double(i) / fs));
    }
    return x;
}

}  // namespace

TEST(Fft, RejectsNonPowerOfTwo) {
    std::vector<std::complex<float>> data(100);
    EXPECT_THROW(sa::fftInPlace(data), std::invalid_argument);
}

TEST(Fft, ImpulseGivesFlatSpectrum) {
    std::vector<std::complex<float>> data(64);
    data[0] = 1.0f;
    sa::fftInPlace(data);
    for (const auto& v : data) {
        EXPECT_NEAR(std::abs(v), 1.0f, 1e-5f);
    }
}

TEST(Fft, MatchesNaiveDft) {
    constexpr std::size_t n = 32;
    std::vector<std::complex<float>> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = {std::cos(0.3f * float(i)), std::sin(1.7f * float(i))};
    }
    const auto input = data;
    sa::fftInPlace(data);

    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> ref = 0;
        for (std::size_t t = 0; t < n; ++t) {
            ref += std::complex<double>(input[t]) *
                   std::polar(1.0, -2.0 * std::numbers::pi * double(k * t) / double(n));
        }
        EXPECT_NEAR(data[k].real(), ref.real(), 1e-4);
        EXPECT_NEAR(data[k].imag(), ref.imag(), 1e-4);
    }
}

TEST(Fft, InverseRestoresInput) {
    constexpr std::size_t n = 64;
    std::vector<std::complex<float>> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = {std::cos(0.3f * float(i)), std::sin(1.7f * float(i))};
    }
    const auto input = data;
    sa::fftInPlace(data);
    sa::inverseFftInPlace(data);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(data[i].real(), input[i].real(), 1e-5f);
        EXPECT_NEAR(data[i].imag(), input[i].imag(), 1e-5f);
    }
}

TEST(SpectrumAnalyzer, BinCentredSineReadsItsAmplitude) {
    constexpr std::size_t n = 1024;
    constexpr double fs = 1024.0;  // 1 Hz per bin
    sa::SpectrumAnalyzer analyzer(n);
    const auto& spectrum = analyzer.compute(sine(n, 64.0, fs, 2.5));

    ASSERT_EQ(spectrum.size(), n / 2 + 1);
    EXPECT_NEAR(spectrum[64], 2.5f, 1e-3f);
    EXPECT_NEAR(spectrum[63], 1.25f, 1e-3f);  // Hann main lobe: half amplitude
    EXPECT_LT(spectrum[10], 1e-4f);
}

TEST(SpectrumAnalyzer, DominantFrequencyBetweenBins) {
    constexpr std::size_t n = 4096;
    constexpr double fs = 10'000.0;  // ~2.44 Hz per bin
    sa::SpectrumAnalyzer analyzer(n);
    const auto& spectrum = analyzer.compute(sine(n, 50.0, fs, 1.0));
    const float f = sa::dominantFrequency(spectrum, float(fs / n));
    EXPECT_NEAR(f, 50.0f, 0.25f);  // well below one bin
}

TEST(SpectrumAnalyzer, RejectsWrongBlockSize) {
    sa::SpectrumAnalyzer analyzer(256);
    std::vector<float> block(100);
    EXPECT_THROW(analyzer.compute(block), std::invalid_argument);
}

namespace {

constexpr std::size_t kEnvN = 4096;
constexpr double kEnvFs = 10'000.0;
constexpr double kEnvBinHz = kEnvFs / kEnvN;  // ~2.44 Hz

}  // namespace

// Carrier at 2500 Hz, 50 % amplitude-modulated at ~87.9 Hz, plus a strong tone
// outside the band. All frequencies sit exactly on bins, so the result is exact.
TEST(EnvelopeAnalyzer, RecoversModulationOfBandLimitedCarrier) {
    const double fm = 36 * kEnvBinHz;
    const double fc = 1024 * kEnvBinHz;
    const double fTone = 20 * kEnvBinHz;
    std::vector<float> x(kEnvN);
    for (std::size_t i = 0; i < kEnvN; ++i) {
        const double t = double(i) / kEnvFs;
        x[i] = static_cast<float>(
            (1.0 + 0.5 * std::cos(2.0 * std::numbers::pi * fm * t)) *
                std::sin(2.0 * std::numbers::pi * fc * t) +
            3.0 * std::sin(2.0 * std::numbers::pi * fTone * t));
    }

    sa::EnvelopeAnalyzer analyzer(kEnvN);
    const auto& env = analyzer.compute(x, float(kEnvBinHz), 2000.0f, 3000.0f);
    ASSERT_EQ(env.size(), kEnvN / 2 + 1);
    EXPECT_NEAR(env[36], 0.5f, 0.01f);   // modulation depth
    EXPECT_LT(env[72], 0.01f);           // pure AM: no harmonics
    EXPECT_LT(env[20], 0.01f);           // the out-of-band tone does not leak in
    EXPECT_NEAR(sa::dominantFrequency(env, float(kEnvBinHz)), fm, 0.5);
}

// A strong tone below the band, off-bin and noise-free. Without windowing before
// the band-pass, its leakage into the band produced a false low-frequency
// envelope line (+50..70 dB above the rest).
TEST(EnvelopeAnalyzer, StrongToneBelowBandDoesNotLeakIn) {
    const auto x = sine(kEnvN, 50.0, kEnvFs, 3.0);
    sa::EnvelopeAnalyzer analyzer(kEnvN);
    const auto& env = analyzer.compute(x, float(kEnvBinHz), 1000.0f, 4000.0f);
    EXPECT_LT(*std::max_element(env.begin() + 2, env.end()), 1e-5f);
}

TEST(EnvelopeAnalyzer, BandWithoutContentGivesFlatEnvelope) {
    const auto x = sine(kEnvN, 1024 * kEnvBinHz, kEnvFs, 1.0);
    sa::EnvelopeAnalyzer analyzer(kEnvN);

    const auto& quiet = analyzer.compute(x, float(kEnvBinHz), 4000.0f, 4500.0f);
    EXPECT_LT(*std::max_element(quiet.begin(), quiet.end()), 1e-3f);

    const auto& aboveNyquist = analyzer.compute(x, float(kEnvBinHz), 6000.0f, 7000.0f);
    EXPECT_EQ(*std::max_element(aboveNyquist.begin(), aboveNyquist.end()), 0.0f);
}

// The bearing case, as in simulator/sensor_sim.py: decaying bursts of a 2.5 kHz
// resonance repeating at 87 Hz. The raw spectrum is dominated by the 50 Hz
// rotation tone; the envelope spectrum reveals the impact rate.
TEST(EnvelopeAnalyzer, FindsImpactRateOfDecayingBursts) {
    constexpr double faultHz = 87.0;
    std::vector<float> x(kEnvN);
    for (std::size_t i = 0; i < kEnvN; ++i) {
        const double t = double(i) / kEnvFs;
        const double sinceImpact = std::fmod(t, 1.0 / faultHz);
        x[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 50.0 * t) +
            std::exp(-800.0 * sinceImpact) * std::sin(2.0 * std::numbers::pi * 2500.0 * sinceImpact));
    }

    sa::SpectrumAnalyzer raw(kEnvN);
    EXPECT_NEAR(sa::dominantFrequency(raw.compute(x), float(kEnvBinHz)), 50.0f, 1.0f);

    sa::EnvelopeAnalyzer analyzer(kEnvN);
    const auto& env = analyzer.compute(x, float(kEnvBinHz), 1000.0f, 4000.0f);
    EXPECT_NEAR(sa::dominantFrequency(env, float(kEnvBinHz)), faultHz, 1.0f);
}

TEST(Statistics, PeakProminenceSeparatesLinesFromNoise) {
    sa::SpectrumAnalyzer analyzer(kEnvN);
    EXPECT_GT(sa::peakProminenceDb(analyzer.compute(sine(kEnvN, 87.0, kEnvFs, 1.0))), 40.0f);

    // The highest of ~2000 noise bins sits only ~12 dB above the median.
    std::mt19937 rng(1);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<float> noise(kEnvN);
    for (float& v : noise) {
        v = gauss(rng);
    }
    EXPECT_LT(sa::peakProminenceDb(analyzer.compute(noise)), 18.0f);
}

TEST(EnvelopeAnalyzer, RejectsWrongBlockSize) {
    sa::EnvelopeAnalyzer analyzer(256);
    std::vector<float> block(100);
    EXPECT_THROW(analyzer.compute(block, 1.0f, 10.0f, 20.0f), std::invalid_argument);
}

TEST(Statistics, RmsAndPeakOfSine) {
    const auto x = sine(10'000, 50.0, 10'000.0, 3.0);
    EXPECT_NEAR(sa::rms(x), 3.0f / std::sqrt(2.0f), 1e-3f);
    EXPECT_NEAR(sa::peakAbs(x), 3.0f, 1e-3f);
}

TEST(Statistics, AmplitudeToDbIsFlooredAtMinus120) {
    EXPECT_NEAR(sa::amplitudeToDb(1.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(sa::amplitudeToDb(0.1f), -20.0f, 1e-4f);
    EXPECT_NEAR(sa::amplitudeToDb(0.0f), -120.0f, 1e-3f);
}
