#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace sa {

// In-place iterative radix-2 Cooley-Tukey FFT. Size must be a power of two
// (throws std::invalid_argument otherwise).
void fftInPlace(std::vector<std::complex<float>>& data);

// Periodic Hann window (the right choice for spectral analysis).
std::vector<float> hannWindow(std::size_t size);

float rms(std::span<const float> x);
float peakAbs(std::span<const float> x);

// Converts a block of samples into a single-sided amplitude spectrum.
// A sine of amplitude A centred on a bin reads as A in that bin.
// Buffers are allocated once; compute() does not allocate.
class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(std::size_t fftSize);

    // `block` must contain exactly fftSize() samples. Returns fftSize()/2 + 1 bins.
    const std::vector<float>& compute(std::span<const float> block);

    std::size_t fftSize() const { return fftSize_; }

private:
    std::size_t fftSize_;
    std::vector<float> window_;
    float amplitudeScale_;
    std::vector<std::complex<float>> work_;
    std::vector<float> spectrum_;
};

// Frequency of the strongest bin (DC excluded), refined with parabolic
// interpolation between neighbouring bins. Returns 0 for an empty/flat spectrum.
float dominantFrequency(std::span<const float> spectrum, float binHz);

}  // namespace sa
