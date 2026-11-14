// Robustness: many sample rates / focus / block sizes / channel layouts / missing sidechain / NaN / silence.
#include "../Source/PhaseAlignCore.h"
#include <cstdio>
#include <vector>
#include <random>
#include <cmath>
int main()
{
    int bad = 0, runs = 0;
    std::mt19937 rng (1); std::uniform_real_distribution<float> u (-1.f, 1.f);
    const double rates[] = { 44100, 48000, 88200, 96000, 192000 };
    const float focuses[] = { 40, 55, 70, 100 };
    for (double fs : rates) for (float foc : focuses) for (int layout = 0; layout < 4; ++layout)
    {
        psy::PhaseAligner a; a.prepare (fs);
        psy::Params p; p.focusHz = foc; p.groupMode = (layout & 1); p.stabilize = true; p.autoTrim = true;
        const int numCh = (layout == 2) ? 1 : 2; const bool haveKick = (layout != 3); const int kickCh = (layout == 2) ? 1 : 2;
        const int total = (int) (fs * 6); long processed = 0; bool finite = true; double maxAbs = 0;
        std::vector<float> L, R, KL, KR;
        const double f0 = fs / std::lround (fs / foc);
        while (processed < total)
        {
            int blk = 1 + (int) (rng() % 8192); if (processed == 0) blk = 1;      // include 1-sample blocks
            blk = std::min<long> (blk, total - processed);
            L.assign ((size_t) blk, 0.f); R = L; KL = L; KR = L;
            for (int i = 0; i < blk; ++i)
            {
                const double t = (double) (processed + i) / fs, ph = std::fmod (t, 0.343);
                float kv = (float) (0.8 * std::sin (2 * psy::kPi * (f0 + 150 * std::exp (-ph / 0.012)) * ph) * std::exp (-ph / 0.08) * (ph < 0.2));
                float bv = (float) (0.4 * std::sin (2 * psy::kPi * f0 * t + 1.0) * (std::fmod (t, 0.0857) < 0.075) * (ph > 0.08));
                if (processed > fs * 3 && processed < fs * 3.5) { kv = 0; bv = 0; }                      // silence gap
                if (processed > fs * 4 && processed < fs * 4 + 400) { kv = bv = std::numeric_limits<float>::quiet_NaN(); } // NaN burst
                if (processed > fs * 5 && processed < fs * 5 + 100) { kv *= 1e6f; bv *= 1e6f; }           // huge spike
                L[(size_t) i] = bv + (p.groupMode ? kv : 0.f) + 1e-5f * u (rng); R[(size_t) i] = L[(size_t) i];
                KL[(size_t) i] = kv; KR[(size_t) i] = kv;
            }
            float* m[2] = { L.data(), R.data() }; const float* k[2] = { KL.data(), KR.data() };
            a.process (m, numCh, haveKick ? k : nullptr, haveKick ? kickCh : 0, blk, p);
            // after the NaN burst everything must recover
            if (processed > fs * 4.8)
                for (int i = 0; i < blk; ++i) { if (! std::isfinite (L[(size_t) i])) finite = false; maxAbs = std::max<double> (maxAbs, std::abs (L[(size_t) i])); }
            processed += blk;
        }
        ++runs;
        if (! finite || maxAbs > 1.0e7) { ++bad; printf ("FAIL fs=%.0f focus=%.0f layout=%d finite=%d maxAbs=%.3g\n", fs, foc, layout, (int) finite, maxAbs); }
    }
    printf ("%d runs, %d failures\n", runs, bad);
    return bad ? 1 : 0;
}
