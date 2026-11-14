// Unit test of the groove detector with synthetic onset times (no audio).
#include "../Source/RhythmTracker.h"
#include <cstdio>
#include <vector>
#include <random>
using namespace psy;
int main()
{
    const double fs = 44100, bpm = 175, beat = 60.0 / bpm * fs, sp = beat / 4;
    std::mt19937 rng (5); std::normal_distribution<double> jit (0.0, 0.0006 * fs);
    int bad = 0, total = 0;
    for (int useHost = 0; useHost < 2; ++useHost)
    {
        RhythmTracker rt; rt.prepare (fs); if (useHost) rt.setHostBpm (bpm);
        // labels per beat: S straight, T tension, F fill
        std::vector<char> lab; std::vector<std::vector<double>> notes;       // note offsets in sixteenths, per beat
        for (int b = 0; b < 64; ++b)
        {
            char L = 'S'; std::vector<double> n = { 1, 2, 3 };
            if (b >= 20 && b < 22) { L = 'F'; n = { 0.5, 1.5, 2.0, 2.5, 3.5 }; }       // 32nd-note turn-around
            if (b == 30) { L = 'T'; n = { 1, 3 }; }                                    // one note missing
            if (b == 40) { L = 'T'; n = { 1, 2, 3, 3.5 }; n.pop_back(); n[2] = 3.4; }  // one note pushed 0.4 sixteenth late
            if (b >= 48 && b < 49) { L = 'F'; n = { 0, 1, 1.5, 2, 2.5, 3, 3.5 }; }
            lab.push_back (L); notes.push_back (n);
        }
        std::vector<int> st (64, -1);
        for (int b = 0; b < 64; ++b)
        {
            const int64_t tk = (int64_t) (b * beat + jit (rng));
            rt.onKick (tk);
            if (b >= 1) st[b - 1] = rt.getState();                     // state describes the beat that just ended
            for (double s : notes[b]) rt.onBass ((int64_t) (b * beat + s * sp + jit (rng)));
        }
        const auto sn = rt.snapshot();
        printf ("%s tempo: bpm %.2f  | pattern mask %d  16th%% %d  density %d%%  onGrid %d%%  running16 %d\n",
                useHost ? "host " : "auto ", sn.bpm, sn.patternMask, sn.sixteenthPct, sn.densityPct, sn.onGridPct, sn.running16);
        for (int b = 6; b < 63; ++b)
        {
            const char L = lab[b]; const int s = st[b];
            const char got = s == RhythmTracker::Straight ? 'S' : s == RhythmTracker::Tension ? 'T' : s == RhythmTracker::Fill ? 'F' : '?';
            const bool ok = (L == got) || (L == 'T' && got == 'F' && false);
            ++total; if (! ok) { ++bad; printf ("   beat %2d: expected %c got %c\n", b, L, got); }
        }
    }
    printf ("%d / %d beats classified correctly\n", total - bad, total);
    return bad ? 1 : 0;
}
