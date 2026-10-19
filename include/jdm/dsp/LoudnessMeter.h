#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>

namespace jdm
{

// EBU R128 / ITU-R BS.1770 Momentary loudness (400 ms window, 100 ms hop,
// ungated). K-weighting is realised as the standard two-stage filter —
// stage 1 high shelf (fc 1681.97 Hz, Q 0.7072, +3.9998 dB) and stage 2 RLB
// highpass (fc 38.135 Hz, Q 0.5003) — designed at the current sample rate
// from the published parameterisation, matching the tabulated 48 kHz
// coefficients. The -0.691 offset calibrates 997 Hz per the spec.
class LoudnessMeter
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = juce::jlimit (1, 2, numChannels);
        designKWeighting();
        subBlockSamples = juce::jmax (1, (int) std::lround (fs * 0.1));
        reset();
    }

    void reset()
    {
        for (auto& ch : states)
            for (auto& st : ch)
                st = {};
        for (auto& e : energies)
            e = 0.0;
        energyIndex = 0;
        acc = 0.0;
        accCount = 0;
        momentaryLufs.store (-100.0f);
    }

    void process (const float* left, const float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            double sum = square (filterSample (0, left[i]));
            if (right != nullptr)
                sum += square (filterSample (1, right[i]));

            acc += sum;
            if (++accCount >= subBlockSamples)
            {
                energies[(size_t) energyIndex] = acc;
                energyIndex = (energyIndex + 1) % 4;
                acc = 0.0;
                accCount = 0;

                const double windowEnergy = energies[0] + energies[1]
                                            + energies[2] + energies[3];
                const double meanSquare = windowEnergy / (4.0 * (double) subBlockSamples);
                momentaryLufs.store (meanSquare > 1.0e-12
                    ? (float) (-0.691 + 10.0 * std::log10 (meanSquare))
                    : -100.0f);
            }
        }
    }

    std::atomic<float> momentaryLufs { -100.0f };

private:
    struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };
    struct State { double z1 = 0.0, z2 = 0.0; };

    static double square (double x) noexcept { return x * x; }

    double filterSample (int ch, float x) noexcept
    {
        double v = (double) x;
        for (int s = 0; s < 2; ++s)
        {
            const auto& c = coeffs[(size_t) s];
            auto& st = states[(size_t) ch][(size_t) s];
            const double y = c.b0 * v + st.z1;
            st.z1 = c.b1 * v - c.a1 * y + st.z2;
            st.z2 = c.b2 * v - c.a2 * y;
            v = y;
        }
        return v;
    }

    void designKWeighting()
    {
        // Stage 1: high shelf, +3.9998 dB, fc 1681.97 Hz, Q 0.7072 (RBJ).
        {
            const double A = std::pow (10.0, 3.9998 / 40.0);
            const double w0 = juce::MathConstants<double>::twoPi * 1681.9744509555319 / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * 0.7071752369554196);
            const double sq = 2.0 * std::sqrt (A) * alpha;

            const double a0 = (A + 1.0) - (A - 1.0) * cw + sq;
            coeffs[0] = { (A * ((A + 1.0) + (A - 1.0) * cw + sq)) / a0,
                          (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw)) / a0,
                          (A * ((A + 1.0) + (A - 1.0) * cw - sq)) / a0,
                          (2.0 * ((A - 1.0) - (A + 1.0) * cw)) / a0,
                          ((A + 1.0) - (A - 1.0) * cw - sq) / a0 };
        }

        // Stage 2: RLB highpass, fc 38.135 Hz, Q 0.5003 (RBJ).
        {
            const double w0 = juce::MathConstants<double>::twoPi * 38.13547087602444 / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * 0.5003270373238773);
            const double k = (1.0 + cw) * 0.5;

            const double a0 = 1.0 + alpha;
            coeffs[1] = { k / a0, -(1.0 + cw) / a0, k / a0,
                          (-2.0 * cw) / a0, (1.0 - alpha) / a0 };
        }
    }

    double fs = 48000.0;
    int channels = 2;
    int subBlockSamples = 4800;
    std::array<Coeffs, 2> coeffs;
    std::array<std::array<State, 2>, 2> states; // [channel][stage]
    std::array<double, 4> energies {};
    int energyIndex = 0;
    double acc = 0.0;
    int accCount = 0;
};

} // namespace jdm
