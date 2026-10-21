#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
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

TEST(Statistics, RmsAndPeakOfSine) {
    const auto x = sine(10'000, 50.0, 10'000.0, 3.0);
    EXPECT_NEAR(sa::rms(x), 3.0f / std::sqrt(2.0f), 1e-3f);
    EXPECT_NEAR(sa::peakAbs(x), 3.0f, 1e-3f);
}
