#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

namespace jdm
{

// A compact send reverb: the cheap subset of Horizon's engine.
//
//   in (L+R) ─► predelay ─► 4 allpass diffusers ─► 8-line FDN ─► L/R taps ─► out
//
// The tank is an 8-line feedback delay network with a Hadamard mixing matrix
// (orthogonal, so every bit of decay comes from the per-line gains, set from
// RT60), a one-pole damping low-pass in each line (highs decay faster), and
// slow, mutually incommensurate modulation of each line's read point to break
// up standing modes. Line lengths are mutually prime-ish and scale with
// `size`; a size change glides (rate-limited read pointers) instead of
// stepping, and each line's decay gain follows its current length. Two
// orthogonal output vectors give decorrelated L/R; `width` blends them toward
// mono. 100 % wet: it is meant for an FX return.
//
// Idle cost: once the input has been silent for longer than the tail takes
// to fall 120 dB, the reverb clears itself and sleeps (outputs silence, does
// no work) until input returns — an FX return nobody sends to is free.
//
// What it leaves out of Horizon: the stepless room-model morph, the decay-rate
// and post EQs, early-reflection modelling and multichannel layouts.
class RoomReverb
{
public:
    struct Params
    {
        float size = 0.5f;          // 0 … 1 (≈ 6 … 40 m path lengths)
        float decaySeconds = 1.2f;  // RT60 at low frequencies
        float predelayMs = 10.0f;   // 0 … 200
        float dampingHz = 6000.0f;  // the in-loop low-pass corner
        float width = 1.0f;         // 0 = mono … 1 = full stereo

        bool operator== (const Params&) const = default;
    };

    static constexpr int numLines = 8;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const int maxLine = (int) std::ceil (maxScale * baseMs[numLines - 1] * 0.001 * fs) + modDepth + 4;
        for (auto& l : lines)
            l.buf.assign ((size_t) nextPow2 (maxLine), 0.0f);
        predelay.assign ((size_t) nextPow2 ((int) std::ceil (0.2 * fs) + 2), 0.0f);
        for (int d = 0; d < numDiffusers; ++d)
        {
            diffusers[(size_t) d].buf.assign ((size_t) juce::jmax (1, (int) std::lround (diffuserMs[d] * 0.001 * fs)), 0.0f);
            diffusers[(size_t) d].pos = 0;
        }
        for (int i = 0; i < numLines; ++i)
        {
            // Modulation: a phasor rotating at the line's rate (no sin per
            // sample), each line starting at its own phase.
            auto& l = lines[(size_t) i];
            const double start = juce::MathConstants<double>::twoPi * i / numLines;
            const double step = juce::MathConstants<double>::twoPi * modRates[i] / fs;
            l.modC = (float) std::cos (start);
            l.modS = (float) std::sin (start);
            l.rotC = (float) std::cos (step);
            l.rotS = (float) std::sin (step);
        }
        configured = false;
        setParams (params);
        for (auto& l : lines)
            l.length = l.target;
        reset();
    }

    void reset() noexcept
    {
        for (auto& l : lines)
        {
            std::fill (l.buf.begin(), l.buf.end(), 0.0f);
            l.lp = 0.0f;
        }
        for (auto& d : diffusers)
            std::fill (d.buf.begin(), d.buf.end(), 0.0f);
        std::fill (predelay.begin(), predelay.end(), 0.0f);
        writePos = 0;
        preWrite = 0;
        asleep = true;
        quietSamples = 0;
    }

    void setParams (const Params& p) noexcept
    {
        if (configured && p == params)
            return;
        params = p;
        configured = true;

        const float scale = minScale + (maxScale - minScale) * juce::jlimit (0.0f, 1.0f, p.size);
        const double rt = juce::jlimit (0.1, 20.0, (double) p.decaySeconds);
        for (int i = 0; i < numLines; ++i)
        {
            auto& l = lines[(size_t) i];
            l.target = (float) (scale * baseMs[i] * 0.001 * fs);
        }
        // -60 dB after rt seconds: a pass of L samples is worth exp (L · k).
        gainPerSample = (float) (-3.0 * std::log (10.0) / (fs * rt));
        tailSamples = (int) std::ceil ((2.0 * rt + 0.2 + maxScale * baseMs[numLines - 1] * 0.001) * fs);
        dampCoeff = (float) std::exp (-juce::MathConstants<double>::twoPi * juce::jlimit (500.0, fs * 0.45, (double) p.dampingHz) / fs);
        predelaySamples = juce::jlimit (0, (int) predelay.size() - 1, (int) std::lround (juce::jlimit (0.0f, 200.0f, p.predelayMs) * 0.001 * fs));
        width = juce::jlimit (0.0f, 1.0f, p.width);
    }

    // `inL/inR` → `outL/outR` (may alias the inputs). 100 % wet.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
    {
        float inPeak = 0.0f;
        for (int i = 0; i < n; ++i)
            inPeak = juce::jmax (inPeak, std::abs (inL[i]), inR != nullptr ? std::abs (inR[i]) : 0.0f);
        if (inPeak > 0.0f)
        {
            quietSamples = 0;
            asleep = false;
        }
        else if (! asleep && (quietSamples += n) > tailSamples)
            reset();
        if (asleep)
        {
            std::fill_n (outL, n, 0.0f);
            std::fill_n (outR, n, 0.0f);
            return;
        }

        const int preMask = (int) predelay.size() - 1;
        const int lineMask = (int) lines[0].buf.size() - 1;
        constexpr float norm = 0.35355339f; // 1/sqrt(8)

        for (auto& ln : lines)
        {
            ln.gain = std::exp (ln.length * gainPerSample);
            const float mag = 1.0f / std::sqrt (ln.modC * ln.modC + ln.modS * ln.modS);
            ln.modC *= mag;
            ln.modS *= mag;
        }

        for (int i = 0; i < n; ++i)
        {
            // Predelay and diffusion on the mono sum.
            predelay[(size_t) preWrite] = 0.5f * (inL[i] + (inR != nullptr ? inR[i] : inL[i]));
            float x = predelay[(size_t) ((preWrite - predelaySamples) & preMask)];
            preWrite = (preWrite + 1) & preMask;
            for (auto& d : diffusers)
            {
                const float delayed = d.buf[(size_t) d.pos];
                const float v = x + diffuseGain * delayed;
                d.buf[(size_t) d.pos] = v;
                x = delayed - diffuseGain * v;
                d.pos = d.pos + 1 >= (int) d.buf.size() ? 0 : d.pos + 1;
            }

            // Read every line (fractional, modulated), damp, apply decay.
            std::array<float, numLines> v {};
            for (int l = 0; l < numLines; ++l)
            {
                auto& ln = lines[(size_t) l];
                ln.length += juce::jlimit (-glide, glide, ln.target - ln.length);
                const float c = ln.modC * ln.rotC - ln.modS * ln.rotS;
                ln.modS = ln.modS * ln.rotC + ln.modC * ln.rotS;
                ln.modC = c;
                const float mod = (float) modDepth * 0.5f * (1.0f + ln.modS);
                const float rp = (float) writePos - ln.length - mod;
                const float fl = std::floor (rp);
                const int i0 = (int) fl & lineMask;
                const float t = rp - fl;
                const float a = ln.buf[(size_t) i0], b = ln.buf[(size_t) ((i0 + 1) & lineMask)];
                const float y = a + t * (b - a);
                ln.lp = y + (ln.lp - y) * dampCoeff;
                v[(size_t) l] = ln.lp * ln.gain;
            }

            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < numLines; ++k)
            {
                l += outSignsL[k] * v[(size_t) k];
                r += outSignsR[k] * v[(size_t) k];
            }

            hadamard (v);
            for (int k = 0; k < numLines; ++k)
                lines[(size_t) k].buf[(size_t) writePos] = v[(size_t) k] * norm + injectSigns[k] * x * norm;
            writePos = (writePos + 1) & lineMask;

            l *= norm;
            r *= norm;
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * width;
            outL[i] = mid + side;
            outR[i] = mid - side;
        }
    }

private:
    static int nextPow2 (int v) noexcept
    {
        int p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    // In-place fast Walsh–Hadamard transform (unnormalised).
    static void hadamard (std::array<float, numLines>& v) noexcept
    {
        for (int h = 1; h < numLines; h <<= 1)
            for (int i = 0; i < numLines; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[(size_t) j], b = v[(size_t) (j + h)];
                    v[(size_t) j] = a + b;
                    v[(size_t) (j + h)] = a - b;
                }
    }

    struct Line
    {
        std::vector<float> buf;
        float length = 1000.0f, target = 1000.0f, gain = 0.5f, lp = 0.0f;
        float modC = 1.0f, modS = 0.0f, rotC = 1.0f, rotS = 0.0f;
    };
    struct Diffuser
    {
        std::vector<float> buf;
        int pos = 0;
    };

    static constexpr int numDiffusers = 4;
    static constexpr float minScale = 0.3f, maxScale = 1.6f;
    static constexpr int modDepth = 6;          // samples, peak to peak
    static constexpr float glide = 0.002f;      // samples per sample
    static constexpr float diffuseGain = 0.62f;
    // Line lengths at size scale 1 (ms): ratios of distinct primes, spread
    // geometrically from 23 to 61 ms.
    static constexpr double baseMs[numLines] = { 23.1, 26.9, 30.7, 34.9, 39.7, 45.1, 52.9, 61.3 };
    static constexpr double diffuserMs[numDiffusers] = { 4.71, 3.59, 12.73, 9.31 };
    static constexpr double modRates[numLines] = { 0.31, 0.37, 0.43, 0.53, 0.59, 0.67, 0.73, 0.83 };
    static constexpr float injectSigns[numLines] = { 1, -1, 1, 1, -1, 1, -1, -1 };
    static constexpr float outSignsL[numLines] = { 1, -1, 1, -1, 1, -1, 1, -1 }; // two Walsh rows: orthogonal
    static constexpr float outSignsR[numLines] = { 1, 1, -1, -1, 1, 1, -1, -1 };

    Params params;
    bool configured = false;
    double fs = 48000.0;
    std::array<Line, numLines> lines;
    std::array<Diffuser, numDiffusers> diffusers;
    std::vector<float> predelay;
    int writePos = 0, preWrite = 0, predelaySamples = 0;
    float dampCoeff = 0.5f, width = 1.0f, gainPerSample = -1.0e-4f;
    int tailSamples = 1 << 20, quietSamples = 0;
    bool asleep = true;
};

} // namespace jdm
