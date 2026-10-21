#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace sa {

// In-place iterative radix-2 Cooley-Tukey FFT. Size must be a power of two
// (throws std::invalid_argument otherwise).
void fftInPlace(std::vector<std::complex<float>>& data);

// Inverse of fftInPlace, including the 1/N scaling.
void inverseFftInPlace(std::vector<std::complex<float>>& data);

// Periodic Hann window (the right choice for spectral analysis).
std::vector<float> hannWindow(std::size_t size);

float rms(std::span<const float> x);
float peakAbs(std::span<const float> x);

// 20*log10(amplitude), floored at -120 dB so silence stays finite.
float amplitudeToDb(float amplitude);

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

// Envelope analysis for bearing diagnostics: band-pass around a structural
// resonance, demodulate (magnitude of the analytic signal) and take the amplitude
// spectrum of the envelope. Repetitive impacts, e.g. rolling elements hitting a
// spall, show up as a peak at their repetition rate and its harmonics even when
// they are buried in the raw spectrum. Band-pass and Hilbert transform are done
// together in the frequency domain. Buffers are allocated once; compute() does
// not allocate.
class EnvelopeAnalyzer {
public:
    explicit EnvelopeAnalyzer(std::size_t fftSize);

    // `block` must contain exactly fftSize() samples. Returns the single-sided
    // amplitude spectrum of the envelope: fftSize()/2 + 1 bins of binHz each.
    // A band that is empty after clamping to (0, Nyquist) gives all zeros.
    const std::vector<float>& compute(std::span<const float> block, float binHz,
                                      float bandLoHz, float bandHiHz);

    std::size_t fftSize() const { return spectrum_.fftSize(); }

private:
    SpectrumAnalyzer spectrum_;  // first: validates fftSize before the buffers are sized
    std::vector<std::complex<float>> work_;
    std::vector<float> envelope_;
};

// Frequency of the strongest bin (DC excluded), refined with parabolic
// interpolation between neighbouring bins. Returns 0 for an empty/flat spectrum.
float dominantFrequency(std::span<const float> spectrum, float binHz);

// How far the strongest bin (DC excluded) stands above the median bin, in dB.
// A periodic component gives a clear line; noise alone stays within a few dB,
// so this tells a real dominantFrequency() from the highest bump of the noise.
float peakProminenceDb(std::span<const float> spectrum);

// Prominence above which an envelope line is taken as real: band noise alone
// measures ~12-14 dB, a weak simulated bearing fault 30 dB or more.
inline constexpr float kClearLineDb = 20.0f;

}  // namespace sa
