#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

namespace jdm
{

// Full loudness measurement per ITU-R BS.1770-4 / EBU R128 (and therefore
// ATSC A/85, which uses the same gated measurement at a different target):
//
//   Momentary   400 ms window, 100 ms hop
//   Short-term  3 s window, 100 ms hop
//   Integrated  gated: absolute -70 LUFS, then relative -10 LU
//   LRA         EBU Tech 3342: short-term distribution, absolute -70 gate,
//               relative -20 LU gate, 95th minus 10th percentile
//
// Threading model: the audio thread runs the K-weighting filters and pushes
// one summed-channel mean-square energy per 100 ms sub-block into a
// lock-free SPSC ring. The message thread calls update() (editor timer),
// which drains the ring into the analysis history and refreshes the
// readouts; the gated integrated/LRA passes recompute every ~250 ms of new
// data rather than per frame. resetIntegration() starts a fresh
// measurement (I + LRA); momentary/short-term keep running.
//
// K-weighting is the standard two-stage filter designed from the published
// parameterisation at any sample rate (same design as the momentary-only
// LoudnessMeter this class supersedes).
//
// Surround (BS.1770-4 table 3): up to 8 channels with per-channel energy
// weights — 1.0 for front channels, 1.41 (~+1.5 dB) for the surrounds, 0
// for the LFE, which is excluded from the measurement. Weights default to
// 1.0 so mono/stereo users are unaffected; callers with a surround layout
// install weights via setChannelWeights().
class LoudnessSuite
{
public:
    static constexpr int maxChannels = 8;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        weights.fill (1.0f);
        designKWeighting();
        subBlockSamples = juce::jmax (1, (int) std::lround (fs * 0.1));
        reset();
    }

    // Per-channel linear energy weights (BS.1770-4: surrounds 1.41, LFE 0).
    // Call after prepare(); missing entries stay at 1.
    void setChannelWeights (const float* w, int count)
    {
        for (int c = 0; c < juce::jmin (count, channels); ++c)
            weights[(size_t) c] = w[c];
    }

    // Audio-thread state only; the analysis history survives (a transport
    // stop/start shouldn't wipe an integrated measurement).
    void reset()
    {
        for (auto& ch : states)
            for (auto& st : ch)
                st = {};
        acc = 0.0;
        accCount = 0;
    }

    // Stereo convenience wrapper — the historic signature. Only valid when
    // prepared with <= 2 channels.
    void process (const float* left, const float* right, int numSamples) noexcept
    {
        const float* chans[2] = { left, right };
        process (chans, numSamples);
    }

    void process (const float* const* chans, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            double sum = 0.0;
            for (int c = 0; c < channels; ++c)
                if (chans[c] != nullptr && weights[(size_t) c] > 0.0f)
                    sum += (double) weights[(size_t) c]
                           * square (filterSample (c, chans[c][i]));

            acc += sum;
            if (++accCount >= subBlockSamples)
            {
                const auto idx = writeIdx.load (std::memory_order_relaxed);
                ring[idx & (ringSize - 1)] = acc / (double) subBlockSamples;
                writeIdx.store (idx + 1, std::memory_order_release);
                acc = 0.0;
                accCount = 0;
            }
        }
    }

    // Message thread ---------------------------------------------------------

    void update()
    {
        const auto w = writeIdx.load (std::memory_order_acquire);

        // If the reader lagged more than a ring's worth (editor closed for
        // ~51 s+), the oldest entries were overwritten — resync and accept
        // the gap rather than reading wrapped data out of order.
        if (w - readIdx > ringSize)
            readIdx = w - ringSize;

        bool fresh = false;
        while (readIdx != w)
        {
            energies.push_back (ring[readIdx & (ringSize - 1)]);
            ++readIdx;
            fresh = true;
        }

        if (! fresh)
            return;

        momentary = windowLoudness (4);
        shortTerm = windowLoudness (30);

        if (++updatesSinceGating >= 8) // ~250 ms of new data at 30 Hz timers
            computeGatedNow();
    }

    // Runs the gated integrated + LRA passes immediately (update() throttles
    // them to ~4 Hz; tests and one-shot analyses call this directly).
    void computeGatedNow()
    {
        updatesSinceGating = 0;
        computeIntegrated();
        computeLra();
    }

    void resetIntegration()
    {
        energies.clear();
        integrated = silence;
        lra = 0.0f;
        momentary = silence;
        shortTerm = silence;
    }

    float momentaryLufs() const { return momentary; }
    float shortTermLufs() const { return shortTerm; }
    float integratedLufs() const { return integrated; }
    float loudnessRangeLu() const { return lra; }

    static constexpr float silence = -100.0f;

private:
    static constexpr uint32_t ringSize = 512; // power of two, ~51 s of headroom

    static double square (double x) noexcept { return x * x; }

    static float toLufs (double meanSquare)
    {
        return meanSquare > 1.0e-12
            ? (float) (-0.691 + 10.0 * std::log10 (meanSquare))
            : silence;
    }

    float windowLoudness (size_t blocks) const
    {
        if (energies.size() < blocks)
            return silence;
        double sum = 0.0;
        for (size_t i = energies.size() - blocks; i < energies.size(); ++i)
            sum += energies[i];
        return toLufs (sum / (double) blocks);
    }

    // Gating blocks: 400 ms at 75 % overlap = the sliding 4-sub-block mean.
    template <typename Fn>
    void forEachGatingBlock (size_t blocks, Fn&& fn) const
    {
        if (energies.size() < blocks)
            return;
        double sum = 0.0;
        for (size_t i = 0; i < blocks; ++i)
            sum += energies[i];
        fn (sum / (double) blocks);
        for (size_t i = blocks; i < energies.size(); ++i)
        {
            sum += energies[i] - energies[i - blocks];
            fn (sum / (double) blocks);
        }
    }

    void computeIntegrated()
    {
        // Absolute gate at -70 LUFS.
        double sum = 0.0;
        size_t count = 0;
        forEachGatingBlock (4, [&] (double msq)
        {
            if (toLufs (msq) > -70.0f)
            {
                sum += msq;
                ++count;
            }
        });
        if (count == 0)
        {
            integrated = silence;
            return;
        }

        // Relative gate 10 LU below the abs-gated energy mean.
        const float relGate = toLufs (sum / (double) count) - 10.0f;
        sum = 0.0;
        count = 0;
        forEachGatingBlock (4, [&] (double msq)
        {
            const float l = toLufs (msq);
            if (l > -70.0f && l > relGate)
            {
                sum += msq;
                ++count;
            }
        });
        integrated = count > 0 ? toLufs (sum / (double) count) : silence;
    }

    void computeLra()
    {
        // Short-term (3 s) distribution, absolute -70 gate.
        scratch.clear();
        double sum = 0.0;
        size_t count = 0;
        forEachGatingBlock (30, [&] (double msq)
        {
            const float l = toLufs (msq);
            if (l > -70.0f)
            {
                scratch.push_back (l);
                sum += msq;
                ++count;
            }
        });
        if (count < 2)
        {
            lra = 0.0f;
            return;
        }

        // Relative gate 20 LU below the energy mean, then p95 - p10.
        const float relGate = toLufs (sum / (double) count) - 20.0f;
        scratch.erase (std::remove_if (scratch.begin(), scratch.end(),
                                       [relGate] (float l) { return l <= relGate; }),
                       scratch.end());
        if (scratch.size() < 2)
        {
            lra = 0.0f;
            return;
        }

        std::sort (scratch.begin(), scratch.end());
        auto percentile = [this] (double p)
        {
            const double pos = p * (double) (scratch.size() - 1);
            const size_t lo = (size_t) pos;
            const double frac = pos - (double) lo;
            return scratch[lo] + (float) frac * (scratch[juce::jmin (lo + 1, scratch.size() - 1)]
                                                 - scratch[lo]);
        };
        lra = juce::jmax (0.0f, percentile (0.95) - percentile (0.10));
    }

    // K-weighting -------------------------------------------------------------

    struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };
    struct State { double z1 = 0.0, z2 = 0.0; };

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
    std::array<std::array<State, 2>, maxChannels> states; // [channel][stage]
    std::array<float, maxChannels> weights { { 1.0f, 1.0f, 1.0f, 1.0f,
                                               1.0f, 1.0f, 1.0f, 1.0f } };

    // Audio -> GUI energy stream.
    std::array<double, ringSize> ring {};
    std::atomic<uint32_t> writeIdx { 0 };
    uint32_t readIdx = 0;
    double acc = 0.0;
    int accCount = 0;

    // Message-thread analysis state.
    std::vector<double> energies;   // one mean-square per 100 ms
    std::vector<float> scratch;
    float momentary = silence, shortTerm = silence, integrated = silence;
    float lra = 0.0f;
    int updatesSinceGating = 0;
};

} // namespace jdm
