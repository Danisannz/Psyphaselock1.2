// =============================================================================
//  ScopeBuffer.h - beat-synchronous waveform capture (no JUCE dependency)
//
//  The audio thread folds kick / bass-before / bass-after onto ONE beat using the
//  host PPQ position.  The last kSlices beats are kept, so the UI can overlay
//  them: if every bass note locks to the kick, the traces stack on top of each other.
//  Single writer (audio thread), single reader (UI thread); relaxed atomics only.
// =============================================================================
#pragma once
#include <atomic>
#include <cmath>
#include <algorithm>

namespace psy
{
class ScopeBuffer
{
public:
    static constexpr int kCols = 720;     // columns per beat
    static constexpr int kSlices = 8;     // beats kept
    static constexpr int kSigs = 3;       // 0 kick, 1 bass before, 2 bass after

    ScopeBuffer() { reset(); }

    // ---- audio thread -----------------------------------------------------------
    void reset()
    {
        for (int s = 0; s < kSlices; ++s) clearSlice (s);
        cur.store (0, std::memory_order_relaxed);
        lastBeat = -1000000000LL;
    }

    void write (double ppqStart, double ppqPerSample, int n,
                const float* kick, const float* bassIn, const float* bassOut)
    {
        int slice = cur.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const double ppq  = ppqStart + (double) i * ppqPerSample;
            const double fl   = std::floor (ppq);
            const long long beat = (long long) fl;
            if (beat != lastBeat)
            {
                lastBeat = beat;
                slice = (slice + 1) % kSlices;
                clearSlice (slice);
                cur.store (slice, std::memory_order_release);
            }
            int col = (int) ((ppq - fl) * (double) kCols);
            col = std::clamp (col, 0, kCols - 1);
            upd (slice, 0, col, kick[i]);
            upd (slice, 1, col, bassIn[i]);
            upd (slice, 2, col, bassOut[i]);
        }
    }

    // ---- UI thread --------------------------------------------------------------
    int currentSlice() const { return cur.load (std::memory_order_acquire); }
    float get (int slice, int sig, int col, int minOrMax) const
    {
        return d[idx (slice, sig, col, minOrMax)].load (std::memory_order_relaxed);
    }

private:
    static constexpr int idx (int s, int g, int c, int k) { return (((s * kSigs + g) * kCols) + c) * 2 + k; }

    void clearSlice (int s)
    {
        for (int g = 0; g < kSigs; ++g)
            for (int c = 0; c < kCols; ++c)
            {
                d[idx (s, g, c, 0)].store ( 1.0e9f, std::memory_order_relaxed);   // min
                d[idx (s, g, c, 1)].store (-1.0e9f, std::memory_order_relaxed);   // max
            }
    }
    void upd (int s, int g, int c, float v)
    {
        auto& mn = d[idx (s, g, c, 0)];
        auto& mx = d[idx (s, g, c, 1)];
        if (v < mn.load (std::memory_order_relaxed)) mn.store (v, std::memory_order_relaxed);
        if (v > mx.load (std::memory_order_relaxed)) mx.store (v, std::memory_order_relaxed);
    }

    std::atomic<float> d[kSlices * kSigs * kCols * 2];
    std::atomic<int>   cur { 0 };
    long long          lastBeat = 0;
};
} // namespace psy
