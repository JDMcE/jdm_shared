#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <complex>

namespace jdm
{

// Biquad designs and a small cascade, extracted from Equinox's band machinery
// (RBJ cookbook sections, Butterworth pole placement for the cuts) for use in
// lightweight channel strips. Equinox itself keeps its own copy for now; the
// maths is the same.

// One normalised section (a0 == 1).
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

namespace biquad
{
    inline constexpr double pi = 3.141592653589793238;

    inline BiquadCoeffs bell (double w0, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha / A;
        return { (1.0 + alpha * A) / a0, (-2.0 * cs) / a0, (1.0 - alpha * A) / a0,
                 (-2.0 * cs) / a0, (1.0 - alpha / A) / a0 };
    }

    inline BiquadCoeffs shelf (double w0, double q, double gainDb, bool high)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double sqA2a = 2.0 * std::sqrt (A) * alpha;
        const double sign = high ? -1.0 : 1.0;

        const double b0 =      A * ((A + 1.0) - sign * (A - 1.0) * cs + sqA2a);
        const double b1 = 2.0 * A * ((A - 1.0) - sign * (A + 1.0) * cs) * sign;
        const double b2 =      A * ((A + 1.0) - sign * (A - 1.0) * cs - sqA2a);
        const double a0 =           (A + 1.0) + sign * (A - 1.0) * cs + sqA2a;
        const double a1 = -2.0 *   ((A - 1.0) + sign * (A + 1.0) * cs) * sign;
        const double a2 =           (A + 1.0) + sign * (A - 1.0) * cs - sqA2a;

        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    // Second-order low-pass (highCut) or high-pass section.
    inline BiquadCoeffs cut2 (double w0, double q, bool highCut)
    {
        const double cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;

        if (highCut)
            return { (1.0 - cs) * 0.5 / a0, (1.0 - cs) / a0, (1.0 - cs) * 0.5 / a0,
                     -2.0 * cs / a0, (1.0 - alpha) / a0 };

        return { (1.0 + cs) * 0.5 / a0, -(1.0 + cs) / a0, (1.0 + cs) * 0.5 / a0,
                 -2.0 * cs / a0, (1.0 - alpha) / a0 };
    }

    // First-order section via the bilinear transform, as a degenerate biquad.
    inline BiquadCoeffs cut1 (double freq, double sampleRate, bool highCut)
    {
        const double w = std::tan (pi * freq / sampleRate);
        const double n = 1.0 / (1.0 + w);

        if (highCut)
            return { w * n, w * n, 0.0, (w - 1.0) * n, 0.0 };

        return { n, -n, 0.0, (w - 1.0) * n, 0.0 };
    }

    inline double magnitudeDb (const BiquadCoeffs& c, double freq, double sampleRate)
    {
        const double w = 2.0 * pi * freq / sampleRate;
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
        return 20.0 * std::log10 (std::abs (num) / juce::jmax (1.0e-30, std::abs (den)));
    }
}

// Up to `maxStages` sections in series, run in transposed direct form II on
// up to two channels. Designing is message-free and allocation-free, so it
// can happen on the audio thread when a setting changes.
template <int maxStages>
class BiquadCascade
{
public:
    void reset() noexcept
    {
        for (auto& ch : state)
            for (auto& s : ch)
                s = {};
    }

    void clearStages() noexcept { numStages = 0; }
    void addStage (const BiquadCoeffs& c) noexcept
    {
        if (numStages < maxStages)
            stages[(size_t) numStages++] = c;
    }

    // Butterworth cut of the given order (1 … 2·maxStages), at `freq`.
    void setButterworth (double freq, double sampleRate, int order, bool highCut) noexcept
    {
        clearStages();
        freq = juce::jlimit (5.0, sampleRate * 0.49, freq);
        order = juce::jlimit (1, maxStages * 2, order);
        const double w0 = 2.0 * biquad::pi * freq / sampleRate;
        for (int k = 0; k < order / 2; ++k)
        {
            const double psi = (order % 2 == 0) ? biquad::pi * (2.0 * k + 1.0) / (2.0 * order)
                                                : biquad::pi * (k + 1.0) / order;
            addStage (biquad::cut2 (w0, 1.0 / (2.0 * std::cos (psi)), highCut));
        }
        if (order % 2 != 0)
            addStage (biquad::cut1 (freq, sampleRate, highCut));
    }

    int getNumStages() const noexcept { return numStages; }

    double magnitudeDb (double freq, double sampleRate) const
    {
        double db = 0.0;
        for (int s = 0; s < numStages; ++s)
            db += biquad::magnitudeDb (stages[(size_t) s], freq, sampleRate);
        return db;
    }

    void process (float* data, int channel, int n) noexcept
    {
        auto& st = state[(size_t) channel];
        for (int i = 0; i < n; ++i)
        {
            double x = data[i];
            for (int s = 0; s < numStages; ++s)
            {
                const auto& c = stages[(size_t) s];
                auto& z = st[(size_t) s];
                const double y = c.b0 * x + z.z1;
                z.z1 = c.b1 * x - c.a1 * y + z.z2;
                z.z2 = c.b2 * x - c.a2 * y;
                x = y;
            }
            data[i] = (float) x;
        }
    }

private:
    struct StageState { double z1 = 0.0, z2 = 0.0; };
    std::array<BiquadCoeffs, (size_t) maxStages> stages {};
    std::array<std::array<StageState, (size_t) maxStages>, 2> state {};
    int numStages = 0;
};

} // namespace jdm
