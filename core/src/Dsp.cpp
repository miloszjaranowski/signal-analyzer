#include "sa/Dsp.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace sa {

void fftInPlace(std::vector<std::complex<float>>& data) {
    const std::size_t n = data.size();
    if (n == 0 || !std::has_single_bit(n)) {
        throw std::invalid_argument("FFT size must be a non-zero power of two");
    }

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // Butterflies. Twiddles are computed in double to keep float error low.
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::size_t half = len / 2;
        for (std::size_t k = 0; k < half; ++k) {
            const auto w = std::complex<float>(std::polar(1.0, angle * static_cast<double>(k)));
            for (std::size_t start = 0; start < n; start += len) {
                const auto u = data[start + k];
                const auto v = data[start + k + half] * w;
                data[start + k] = u + v;
                data[start + k + half] = u - v;
            }
        }
    }
}

std::vector<float> hannWindow(std::size_t size) {
    std::vector<float> w(size);
    for (std::size_t i = 0; i < size; ++i) {
        w[i] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                  static_cast<double>(size))));
    }
    return w;
}

float rms(std::span<const float> x) {
    if (x.empty()) {
        return 0.0f;
    }
    const double sumSq = std::accumulate(x.begin(), x.end(), 0.0,
                                         [](double acc, float v) { return acc + double(v) * v; });
    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(x.size())));
}

float peakAbs(std::span<const float> x) {
    float peak = 0.0f;
    for (float v : x) {
        peak = std::max(peak, std::abs(v));
    }
    return peak;
}

SpectrumAnalyzer::SpectrumAnalyzer(std::size_t fftSize)
    : fftSize_(fftSize),
      window_(hannWindow(fftSize)),
      work_(fftSize),
      spectrum_(fftSize / 2 + 1) {
    if (fftSize < 2 || !std::has_single_bit(fftSize)) {
        throw std::invalid_argument("FFT size must be a power of two >= 2");
    }
    // Single-sided amplitude: 2 / (sum of window) undoes both the 1/N energy
    // spread and the window's coherent gain.
    const double windowSum = std::accumulate(window_.begin(), window_.end(), 0.0);
    amplitudeScale_ = static_cast<float>(2.0 / windowSum);
}

const std::vector<float>& SpectrumAnalyzer::compute(std::span<const float> block) {
    if (block.size() != fftSize_) {
        throw std::invalid_argument("Block size must equal FFT size");
    }
    for (std::size_t i = 0; i < fftSize_; ++i) {
        work_[i] = {block[i] * window_[i], 0.0f};
    }
    fftInPlace(work_);

    const std::size_t bins = spectrum_.size();
    for (std::size_t k = 0; k < bins; ++k) {
        float a = std::abs(work_[k]) * amplitudeScale_;
        if (k == 0 || k == bins - 1) {
            a *= 0.5f;  // DC and Nyquist are not mirrored
        }
        spectrum_[k] = a;
    }
    return spectrum_;
}

float dominantFrequency(std::span<const float> spectrum, float binHz) {
    if (spectrum.size() < 3) {
        return 0.0f;
    }
    const auto maxIt = std::max_element(spectrum.begin() + 1, spectrum.end());
    const auto k = static_cast<std::size_t>(std::distance(spectrum.begin(), maxIt));
    if (*maxIt <= 0.0f) {
        return 0.0f;
    }
    float offset = 0.0f;
    if (k + 1 < spectrum.size()) {
        // Parabolic fit on log magnitude: accurate to a few % of a bin for Hann.
        constexpr float kFloor = 1e-12f;
        const float a = std::log(std::max(spectrum[k - 1], kFloor));
        const float b = std::log(std::max(spectrum[k], kFloor));
        const float c = std::log(std::max(spectrum[k + 1], kFloor));
        const float denom = a - 2.0f * b + c;
        if (std::abs(denom) > 1e-12f) {
            offset = std::clamp(0.5f * (a - c) / denom, -0.5f, 0.5f);
        }
    }
    return (static_cast<float>(k) + offset) * binHz;
}

}  // namespace sa
