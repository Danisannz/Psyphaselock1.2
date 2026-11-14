// =============================================================================
//  RhythmTracker.h  -  kick/bass groove detector (no JUCE dependency, no allocation)
//
//  Fed with onset TIMES only (sample counter), so it costs nothing per sample.
//  * Tempo: host BPM when given, otherwise the median kick-to-kick interval.
//  * Every bass onset is snapped to the 16th grid that starts at the last kick:
//        pos = (tBass - tKick) / (beat/4)    slot = round(pos) mod 4    err = pos - round(pos)
//  * Every beat is stored as a 4-bit mask (which 16ths had a note).  The most frequent
//    mask of the last 8 beats is the "pattern".  Per beat:
//        STRAIGHT : same mask as the pattern, every note on the grid (|err| < 0.2 sixteenth)
//        TENSION  : one note too many / missing / pushed off the grid  (micro tension)
//        FILL     : 2+ differences, kick lost or kick timing irregular    (turn-around)
//  * sixteenthPct = share of the last 8 beats in which the bass ran 3-4 notes on the 16th
//    grid ("running in 16ths").
// =============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace psy
{
class RhythmTracker
{
public:
    enum State { NoData = 0, Straight = 1, Tension = 2, Fill = 3 };

    struct Snapshot
    {
        int   state        = NoData;
        int   sixteenthPct = 0;      // beats (of the last 8) that ran 3-4 on-grid 16ths
        int   densityPct   = 0;      // occupied 16th slots
        int   onGridPct    = 0;      // bass onsets within +-0.2 sixteenth of the grid
        int   patternMask  = 0;      // bit0 = on the kick, bit1 = "e", bit2 = "&", bit3 = "a"
        int   regularityPct = 0;     // beats equal to the pattern
        float bpm          = 0.0f;
        bool  running16    = false;  // true when the bass runs in 16ths most of the time
    };

    struct NoteInfo
    {
        bool   valid    = false;     // grid known
        int    slot     = 0;         // 0..3
        double err      = 0.0;       // in sixteenths, -0.5..0.5
        bool   onGrid   = false;
        bool   expected = true;      // slot belongs to the current pattern
    };

    void prepare (double sampleRate) { fs = sampleRate; reset(); }

    void reset()
    {
        nIoi = 0; ioiPos = 0; beat = 0.0; tempoOk = false;
        lastKick = -1; curMask = nextMask = 0; curOn = curOff = nextOn = nextOff = 0; curKickOk = true;
        nRec = 0; recPos = 0; modal = 0; state = NoData; cleanStreak = 0;
        snap = Snapshot();
    }

    void setHostBpm (double bpm) { hostBpm = (bpm >= 60.0 && bpm <= 260.0) ? bpm : 0.0; }

    // ---- events ----------------------------------------------------------------
    void onKick (int64_t t)
    {
        bool kickOk = true;
        if (lastKick >= 0)
        {
            const double ioi = (double) (t - lastKick);
            if (ioi > 0.20 * fs && ioi < 1.00 * fs)
            {
                ioiBuf[ioiPos] = ioi; ioiPos = (ioiPos + 1) & 7; nIoi = std::min (nIoi + 1, 8);
            }
            updateTempo();
            if (beat > 0.0) kickOk = std::abs (ioi - beat) < 0.14 * beat;
            else            kickOk = false;

            // finalize the beat that just ended
            BeatRec r;
            r.mask = curMask; r.nOn = curOn; r.nOff = curOff; r.kickOk = kickOk && curKickOk;
            rec[recPos] = r; recPos = (recPos + 1) & 7; nRec = std::min (nRec + 1, 8);
            evaluate();
        }
        curMask = nextMask; curOn = nextOn; curOff = nextOff;
        nextMask = 0; nextOn = nextOff = 0; curKickOk = true;
        lastKick = t;
    }

    NoteInfo onBass (int64_t t)
    {
        NoteInfo n;
        if (lastKick < 0 || ! tempoOk || beat <= 0.0) return n;
        const double sp  = beat * 0.25;
        const double pos = (double) (t - lastKick) / sp;
        if (pos < -0.5 || pos > 4.6) { curKickOk = false; return n; }       // kick missing -> a turn-around
        const int    r   = (int) std::lround (pos);
        n.valid = true;
        n.err   = pos - (double) r;
        n.slot  = ((r % 4) + 4) % 4;
        n.onGrid = std::abs (n.err) < 0.2;
        if (modal != 0 && nRec >= 3) n.expected = ((modal >> n.slot) & 1) != 0;

        // an on-grid note exactly on the next kick belongs to the next beat; anything else stays in this beat
        if (r >= 4 && n.onGrid) { nextMask |= 1; ++nextOn; }
        else { curMask |= (1 << std::min (std::max (r, 0), 3)); if (n.onGrid) ++curOn; else ++curOff; }
        return n;
    }

    // call once per audio block: a long silence of the kick means breakdown -> no data
    void tick (int64_t t)
    {
        if (lastKick >= 0 && beat > 0.0 && (double) (t - lastKick) > 2.2 * beat)
        {
            state = NoData; snap.state = NoData; snap.running16 = false;
        }
    }

    Snapshot snapshot() const { return snap; }
    int      getState() const { return state; }

private:
    struct BeatRec { int mask = 0; int nOn = 0; int nOff = 0; bool kickOk = true; };

    static int popcnt4 (int v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1); }

    void updateTempo()
    {
        if (hostBpm > 0.0) { beat = 60.0 / hostBpm * fs; tempoOk = true; return; }
        if (nIoi < 3) { tempoOk = false; beat = 0.0; return; }
        double s[8]; for (int i = 0; i < nIoi; ++i) s[i] = ioiBuf[i];
        std::sort (s, s + nIoi);
        const double med = s[nIoi / 2];
        double sum = 0.0; int cnt = 0;
        for (int i = 0; i < nIoi; ++i) if (std::abs (s[i] - med) < 0.06 * med) { sum += s[i]; ++cnt; }
        tempoOk = cnt >= std::max (3, (int) std::ceil (0.6 * nIoi));
        beat = tempoOk ? sum / (double) cnt : 0.0;
    }

    void evaluate()
    {
        snap.bpm = (beat > 0.0) ? (float) (60.0 * fs / beat) : 0.0f;
        if (nRec < 3 || ! tempoOk) { state = NoData; fill(); return; }

        // most frequent mask of the stored beats (ties -> the most recent one)
        int hist[16] = {};
        for (int i = 0; i < nRec; ++i) ++hist[rec[i].mask & 15];
        const BeatRec& last = rec[(recPos + 7) & 7];
        int best = last.mask & 15;
        for (int m = 0; m < 16; ++m) if (hist[m] > hist[best]) best = m;
        modal = best;

        if (modal == 0) { state = NoData; fill(); return; }

        const int dev = popcnt4 ((last.mask ^ modal) & 15);
        if (! last.kickOk)                       state = Fill;
        else if (dev == 0 && last.nOff == 0)     state = (hist[modal] * 2 >= nRec) ? Straight : Tension;
        else if (dev <= 1 && last.nOff <= 1)     state = Tension;
        else                                     state = Fill;
        fill();
    }

    void fill()
    {
        int run = 0, occ = 0, on = 0, off = 0, eq = 0;
        for (int i = 0; i < nRec; ++i)
        {
            occ += popcnt4 (rec[i].mask); on += rec[i].nOn; off += rec[i].nOff;
            if (popcnt4 (rec[i].mask) >= 3 && rec[i].nOff == 0) ++run;
            if ((rec[i].mask & 15) == modal) ++eq;
        }
        const int n = std::max (nRec, 1);
        snap.state = state;
        snap.sixteenthPct = (100 * run) / n;
        snap.densityPct   = (100 * occ) / (4 * n);
        snap.onGridPct    = (on + off) > 0 ? (100 * on) / (on + off) : 0;
        snap.regularityPct = (100 * eq) / n;
        snap.patternMask  = modal;
        snap.running16    = (state != NoData) && snap.sixteenthPct >= 60;
    }

    double  fs = 44100.0, hostBpm = 0.0, beat = 0.0;
    double  ioiBuf[8] = {};
    int     nIoi = 0, ioiPos = 0;
    bool    tempoOk = false, curKickOk = true;
    int64_t lastKick = -1;
    int     curMask = 0, nextMask = 0, curOn = 0, curOff = 0, nextOn = 0, nextOff = 0;
    BeatRec rec[8];
    int     nRec = 0, recPos = 0, modal = 0, state = NoData, cleanStreak = 0;
    Snapshot snap;
};
} // namespace psy
