// =============================================================================
//  PhaseAlignCore.h  -  Kick/Bass phase alignment engine (no JUCE dependency)
//
//  Measurement
//  -----------
//  * KICK reference: the phase of the kick is taken from a "settled" window - after its
//    pitch sweep, while it is still strong (frequency of the phasor == focus frequency).
//    That phase is held, so bass notes that start when the kick has faded (or is already
//    gone) are still aligned to the kick's continuation ("ghost kick").
//  * BASS phase: Hann-weighted 2-period sliding DFT (3 bins, ~6 MACs / sample) = immune
//    to the note's attack ramp and decay.  A fast 1-period estimate is used first and is
//    replaced by the precise one as soon as it exists; very short notes keep the fast one.
//  * The correction is applied in the gap BEFORE the note reaches the output
//    (65 ms look-ahead = reported latency), by a pure all-pass (+ polarity flip).
//  Groove
//  ------
//  * RhythmTracker (kick/bass onsets) tells STRAIGHT / TENSION / FILL and how much of the
//    time the bass runs in 16ths.  While STRAIGHT, every 16th slot keeps a circular memory
//    of its phase; a measurement that agrees with a consistent memory is replaced by the
//    memory (no jitter).  A note that is off the grid / unexpected, or a FILL, bypasses it.
//
//  Track : main = bass only,  sidechain = kick.   out = aligned bass
//  Group : main = kick+bass,  sidechain = kick.   bass = main - trim*kick;
//          out = main + wet * (rot(bass) - bass)   (everything else untouched)
// =============================================================================
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "RhythmTracker.h"

namespace psy
{
constexpr double kPi = 3.14159265358979323846;

struct Params
{
    bool  groupMode   = true;
    float focusHz     = 55.0f;    // 40..100  (shared pitch of kick and bass)
    bool  autoAlign   = true;
    float manualDeg   = 0.0f;     // 0..360 bass phase lag when auto is off
    float targetDeg   = 0.0f;     // -90..90 wanted bass phase relative to kick
    float thresholdDb = -45.0f;   // activity gate for kick / bass
    float wet         = 1.0f;     // 0..1
    float kickTrimDb  = 0.0f;     // group mode: level match of sidechain kick (manual)
    float settleMs    = 0.0f;     // 0..40 extra wait before measuring (limited by the look-ahead)
    bool  ghostRef    = true;     // bass notes with no kick: follow the phase of the last kick
    bool  stabilize   = true;     // groove-aware per-slot phase memory
    bool  autoTrim    = true;     // group mode: measure the kick level in the group by itself
    float hostBpm     = 0.0f;     // 0 = detect the tempo from the kick
};

class PhaseAligner
{
public:
    static constexpr double kLatencySeconds = 0.065;
    static constexpr double kLeadSeconds    = 0.008;     // correction lands this long before the note
    static constexpr double kQ              = 0.7071067811865476;

    struct Tap { float* kick = nullptr; float* bassIn = nullptr; float* bassOut = nullptr; };

    struct Meters
    {
        std::atomic<float>    deltaDeg   { 0.f };   // measured bass-vs-kick phase before correction
        std::atomic<float>    appliedDeg { 0.f };   // correction applied (0..360, incl. polarity)
        std::atomic<float>    kickDb     { -100.f };
        std::atomic<float>    bassDb     { -100.f };
        std::atomic<int>      locked     { 0 };     // 0 none, 1 precise, 2 fast (short note)
        std::atomic<uint32_t> hits       { 0 };     // +1 on every correction applied
        std::atomic<int>      rhythmState   { 0 };  // RhythmTracker::State
        std::atomic<int>      sixteenthPct  { 0 };
        std::atomic<int>      densityPct    { 0 };
        std::atomic<int>      onGridPct     { 0 };
        std::atomic<int>      patternMask   { 0 };
        std::atomic<int>      memoryUsed    { 0 };  // 1 while the last correction came from the slot memory
        std::atomic<float>    bpm        { 0.f };
        std::atomic<float>    trimDb     { 0.f };   // kick trim in use (group mode)
    } meters;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        latency = (int) std::lround (kLatencySeconds * fs);
        leadSamples = (int) std::lround (kLeadSeconds * fs);
        for (int c = 0; c < 2; ++c)
        {
            dX[c].assign ((size_t) latency + 1, 0.f);
            dK[c].assign ((size_t) latency + 1, 0.f);
        }
        detLp   = 1.0 - std::exp (-2.0 * kPi * 200.0 / fs);
        envAtk  = 1.0 - std::exp (-1.0 / (0.0003 * fs));
        envRel  = 1.0 - std::exp (-1.0 / (0.0015 * fs));
        polCoef = 1.0 - std::exp (-1.0 / (0.0015 * fs));
        lagCoef = 1.0 - std::exp (-1.0 / (0.0015 * fs));
        wetCoef = 1.0 - std::exp (-1.0 / (0.0050 * fs));
        trimBegin = (int64_t) (0.006 * fs); trimEnd = (int64_t) (0.036 * fs);
        trimCoef = 1.0 - std::exp (-1.0 / (0.050 * fs));
        rhythm.prepare (fs);
        reset();
    }

    int getLatencySamples() const { return latency; }

    void reset()
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (dX[c].begin(), dX[c].end(), 0.f);
            std::fill (dK[c].begin(), dK[c].end(), 0.f);
            ic1[c] = ic2[c] = 0.0;
        }
        wp = 0; N = 0; M = 0;                    // forces analysis rebuild
        kLp1 = kLp2 = bLp1 = bLp2 = mLp1 = mLp2 = 0.0;
        kEnv = bEnv = 0.0; bPeak = bMin = 0.0;
        kGate = bGate = false; bAge = 0;
        kNote = NoteGate();
        lagTarget = 0.0; polTarget = 1.0;
        lagSm = 0.0; polSm = 1.0; lagUsed = -1.0; ctl = 0;
        wetSm = 1.0; trimSm = 0.0; trimAuto = 1.0; trimAutoValid = false; trimNum = trimDen = 0.0;
        trimWinActive = false; trimHistN = trimHistPos = 0;
        ghostSAge = ghostWAge = 1 << 30; hitPeak = 0.0; hasPrevZk = false;
        ghostSValid = ghostWValid = ghostPValid = lastGoodValid = false; hitOnset = lastGoodOnset = 0;
        pendValid = false; noteFastDone = noteHannDone = false; fastDelta = 0.0; fastValid = false;
        nowSamp = 0;
        rhythm.reset();
        curNote = RhythmTracker::NoteInfo();
        clearMemory();
    }

    // In-place. main: numCh (1..2) channels. kick: kickCh channels or nullptr.
    void process (float* const* main, int numCh, const float* const* kick, int kickCh,
                  int n, const Params& p, const Tap* tap = nullptr)
    {
        if (fs <= 0.0 || numCh < 1 || n <= 0) return;
        numCh = std::min (numCh, 2);

        setupFocus (p.focusHz);

        const bool haveKick = (kick != nullptr && kickCh > 0);
        autoActive = p.autoAlign && haveKick;
        group      = p.groupMode;
        stabOn     = p.stabilize;
        autoTrimOn = p.autoTrim && p.groupMode;
        const double trimManual = p.groupMode ? dbToLin (p.kickTrimDb) : 0.0;
        thrOn  = dbToLin (p.thresholdDb);
        thrOff = thrOn * 0.5;
        target = (double) p.targetDeg;
        ghostOn = p.ghostRef;
        rhythm.setHostBpm (p.hostBpm);

        // timing budget: the precise estimate must be ready before the correction has to be applied
        applyOffset = latency - leadSamples;
        const int lat07 = (int) (0.0007 * fs);
        const int room  = applyOffset - (M + lat07) - 2;
        hannOk = room >= 0;
        const int settleReq = (int) (std::clamp (p.settleMs, 0.0f, 40.0f) * 0.001 * fs);
        settleSamples = std::max (0, std::min (settleReq, hannOk ? room : applyOffset - (N + lat07) - 2));
        needFast = N + lat07 + settleSamples;
        needHann = M + lat07 + settleSamples;

        const double wetT = (double) std::clamp (p.wet, 0.0f, 1.0f);

        if (! autoActive)
        {
            const double m = wrap360 ((double) p.manualDeg);
            if (m < 180.0) { polTarget =  1.0; lagTarget = m; }
            else           { polTarget = -1.0; lagTarget = m - 180.0; }
            meters.appliedDeg.store ((float) m);
            meters.locked.store (0);
            pendValid = false;
        }

        const size_t dsz = (size_t) latency + 1;

        for (int i = 0; i < n; ++i)
        {
            // a NaN / Inf from the host must never poison the filters: sanitise every input sample
            const float x0 = clean (main[0][i]);
            const float x1 = numCh > 1 ? clean (main[1][i]) : x0;
            float k0 = 0.f, k1 = 0.f;
            if (haveKick) { k0 = clean (kick[0][i]); k1 = kickCh > 1 ? clean (kick[1][i]) : k0; }

            // group-mode kick level: manual or measured, slewed smoothly
            const double trimTarget = ! group ? 0.0 : ((autoTrimOn && trimAutoValid) ? trimAuto : trimManual);
            trimSm += trimCoef * (trimTarget - trimSm);
            const float trim = (float) trimSm;

            // analysis on the un-delayed signals
            const float km = 0.5f * (k0 + k1);
            const float mm = 0.5f * (x0 + x1);
            const float bm = mm - trim * km;
            analyse (km, bm, mm, haveKick);

            // look-ahead delay
            dX[0][(size_t) wp] = x0;  dX[1][(size_t) wp] = x1;
            dK[0][(size_t) wp] = k0;  dK[1][(size_t) wp] = k1;
            size_t rp = (size_t) wp + 1; if (rp >= dsz) rp = 0;
            const float xd0 = dX[0][rp], xd1 = dX[1][rp];
            const float kd0 = dK[0][rp], kd1 = dK[1][rp];
            if (++wp >= (int) dsz) wp = 0;

            const float b0 = xd0 - trim * kd0;
            const float b1 = xd1 - trim * kd1;

            // smooth control values
            lagSm += lagCoef * (lagTarget - lagSm);
            polSm += polCoef * (polTarget - polSm);
            wetSm += wetCoef * (wetT - wetSm);
            if ((++ctl & 15) == 0 && std::abs (lagSm - lagUsed) > 0.01)
            {
                setAllpass (lagSm);
                lagUsed = lagSm;
            }

            // all-pass rotation (ZDF state-variable filter)
            const float r0 = (float) (polSm * allpass (0, b0));
            main[0][i] = xd0 + (float) wetSm * (r0 - b0);
            float bo = main[0][i] - xd0 + b0;                  // bass after correction (ch 0)
            if (numCh > 1)
            {
                const float r1 = (float) (polSm * allpass (1, b1));
                main[1][i] = xd1 + (float) wetSm * (r1 - b1);
                bo = 0.5f * (bo + (main[1][i] - xd1 + b1));
            }
            if (tap != nullptr)
            {
                tap->kick[i]    = 0.5f * (kd0 + kd1);
                tap->bassIn[i]  = 0.5f * (b0 + b1);
                tap->bassOut[i] = bo;
            }
        }

        if (! std::isfinite (ic1[0] + ic2[0] + ic1[1] + ic2[1] + lagSm + polSm + trimSm + kEnv + bEnv))
        {
            ic1[0] = ic2[0] = ic1[1] = ic2[1] = 0.0; lagSm = lagTarget = 0.0; polSm = polTarget = 1.0; trimSm = 0.0;
            kLp1 = kLp2 = bLp1 = bLp2 = mLp1 = mLp2 = kEnv = bEnv = 0.0; lagUsed = -1.0;
        }
        rhythm.tick (nowSamp);
        publishRhythm();
        meters.kickDb.store ((float) linToDb (kEnv));
        meters.bassDb.store ((float) linToDb (bEnv));
        meters.trimDb.store ((float) linToDb (std::max (trimSm, 1e-6)));
    }

private:
    // ------------------------------------------------------------------ small helpers
    static float clean (float v) { return std::isfinite (v) ? std::clamp (v, -1.0e6f, 1.0e6f) : 0.0f; }
    static double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
    static double linToDb (double x)  { return 20.0 * std::log10 (std::max (x, 1e-6)); }
    static double wrap360 (double d)  { d = std::fmod (d, 360.0); return d < 0.0 ? d + 360.0 : d; }
    static double wrap180 (double d)  { d = std::fmod (d + 180.0, 360.0); if (d < 0.0) d += 360.0; return d - 180.0; }

    static double medianOf (double* v, int n)      // n <= 8, sorts in place
    {
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && v[j - 1] > v[j]; --j) std::swap (v[j - 1], v[j]);
        return v[n / 2];
    }

    struct Cx { double re = 0.0, im = 0.0; };
    static Cx cmul (Cx a, Cx b)   { return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }
    static Cx cmulc (Cx a, Cx b)  { return { a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im }; }   // a * conj (b)

    // relative-peak note detector: closes at 20 % of the note's peak, re-opens on a real rise
    struct NoteGate
    {
        bool open = false; double peak = 0.0, minv = 0.0;
        int update (double env, double thrOn, double thrOff)
        {
            if (open)
            {
                peak = std::max (peak, env);
                if (env < std::max (thrOff, 0.2 * peak)) { open = false; minv = env; return -1; }
            }
            else
            {
                minv = std::min (minv, env);
                if (env > thrOn && env > 2.0 * minv) { open = true; peak = env; return 1; }
            }
            return 0;
        }
    };

    // ------------------------------------------------------------------ setup
    void setupFocus (float focusHz)
    {
        focusHz = std::clamp (focusHz, 20.0f, 200.0f);
        const int n = std::max (16, (int) std::lround (fs / (double) focusHz));
        if (n == N) return;
        N = n; M = 2 * N;
        f0 = fs / (double) N;                    // one window = exactly one period
        cosT.resize ((size_t) N); sinT.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const double th = 2.0 * kPi * (double) i / (double) N;
            cosT[(size_t) i] = (float) std::cos (th);
            sinT[(size_t) i] = (float) std::sin (th);
        }
        for (int k = 0; k < 3; ++k)
        {
            twC[k].resize ((size_t) M); twS[k].resize ((size_t) M);
            for (int m = 0; m < M; ++m)
            {
                const double th = 2.0 * kPi * (double) (k + 1) * (double) m / (double) M;
                twC[k][(size_t) m] = (float) std::cos (th);
                twS[k][(size_t) m] = (float) std::sin (th);
            }
        }
        ring.assign ((size_t) N * 4, 0.f);
        bRing.assign ((size_t) M, 0.f);
        const int nq = std::max (2, N / 4);          // quarter period
        kq.assign ((size_t) nq, 0.0); bq.assign ((size_t) nq, 0.0); qpos = 0;
        kEnv = bEnv = 0.0; bPeak = bMin = 0.0;
        sKc = sKs = sBc = sBs = 0.0;
        for (int k = 0; k < 3; ++k) Yr[k] = Yi[k] = 0.0;
        idx = 0; hpos = 0; silentRun = 0;
        kGate = bGate = false; bAge = 0; kNote = NoteGate();
        stabStep = std::max (4, N / 2); stabCount = 0; hasPrevZk = false; stableRun = 0;
        ghostSAge = ghostWAge = 1 << 30; hitPeak = 0.0;
        ghostSValid = ghostWValid = ghostPValid = lastGoodValid = false;
        pendValid = false; noteFastDone = noteHannDone = false; fastValid = false;
        tanF0 = std::tan (kPi * f0 / fs);
        lagUsed = -1.0;                          // force coefficient refresh
        clearMemory();
    }

    // all-pass whose phase lag at f0 equals lagDeg (0..180)
    void setAllpass (double lagDeg)
    {
        lagDeg = std::clamp (lagDeg, 0.02, 180.0);
        double u;                                // u = f0/fc in (0,1]
        if (lagDeg >= 179.99) u = 1.0;
        else
        {
            const double t = std::tan (lagDeg * kPi / 360.0);
            u = (-1.0 / kQ + std::sqrt (1.0 / (kQ * kQ) + 4.0 * t * t)) / (2.0 * t);
        }
        const double g = tanF0 / u;              // pre-warped: exact phase at f0
        k  = 1.0 / kQ;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    inline double allpass (int ch, double x)
    {
        const double v3 = x - ic2[ch];
        const double v1 = a1 * ic1[ch] + a2 * v3;
        const double v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2.0 * v1 - ic1[ch];
        ic2[ch] = 2.0 * v2 - ic2[ch];
        return x - 2.0 * k * v1;
    }

    // ------------------------------------------------------------------ groove memory
    struct SlotMem { double re = 0, im = 0, w = 0; int n = 0, out = 0; };
    void clearMemory() { for (auto& m : mem) m = SlotMem(); }

    bool memoryConsistent (const SlotMem& m) const
    {
        return m.n >= 3 && m.w > 0.5 && std::hypot (m.re, m.im) / m.w >= 0.95;
    }

    // confident measurement: remember it (a far outlier of a consistent history = a new note: re-seed after two)
    void memoryLearn (double deltaDeg, int slot)
    {
        SlotMem& m = mem[(size_t) (slot & 3)];
        const double a = deltaDeg * kPi / 180.0, cr = std::cos (a), ci = std::sin (a);
        if (memoryConsistent (m)
            && std::abs (wrap180 (deltaDeg - std::atan2 (m.im, m.re) * 180.0 / kPi)) > 30.0)
        {
            if (++m.out >= 2) { m = SlotMem(); m.re = cr; m.im = ci; m.w = 1.0; m.n = 1; }
            return;
        }
        m.out = 0;
        constexpr double lam = 0.75;
        m.re = lam * m.re + cr; m.im = lam * m.im + ci; m.w = lam * m.w + 1.0;
        m.n = std::min (m.n + 1, 1000);
    }

    void publishRhythm()
    {
        const auto s = rhythm.snapshot();
        meters.rhythmState.store (s.state);
        meters.sixteenthPct.store (s.sixteenthPct);
        meters.densityPct.store (s.densityPct);
        meters.onGridPct.store (s.onGridPct);
        meters.patternMask.store (s.patternMask);
        meters.bpm.store (s.bpm);
    }

    // ------------------------------------------------------------------ note handling
    void onKickOnset()
    {
        rhythm.onKick (nowSamp);
        hitPeak = 0.0;
        // Every hit starts at a different point of the focus oscillator, so the previous hit's settled phase is
        // only valid after rotating it by the time between the two onsets (kicks are the same one-shot each time).
        ghostPValid = false;
        if (lastGoodValid && (nowSamp - lastGoodOnset) < (int64_t) (3.0 * fs))
        {
            const double ang = -2.0 * kPi * (double) ((nowSamp - lastGoodOnset) % (int64_t) N) / (double) N;
            ghostP = cmul (lastGood, { std::cos (ang), std::sin (ang) });
            ghostPValid = true;
        }
        ghostSValid = ghostWValid = false; stableRun = 0;
        hitOnset = nowSamp;
        // auto trim: least-squares fit of (main) on (kick) in a short fixed window right after the onset
        trimWinActive = autoTrimOn; trimWinStart = nowSamp; trimNum = trimDen = 0.0;
        publishRhythm();
    }

    // Per-hit estimate of "how much of the sidechain kick is inside the main signal".
    // The median of the last 8 hits is used, and only while the hits agree (so a bass that starts with the
    // kick - which spoils the fit - makes the estimate invalid and the manual KICK TRIM is used instead).
    void finishTrimWindow()
    {
        const double minDen = 0.1 * std::pow (10.0 * thrOn, 2.0) * (double) (trimEnd - trimBegin);
        if (trimDen > minDen)
        {
            const double v = trimNum / trimDen;
            if (std::isfinite (v) && v > 0.05 && v < 8.0)
            {
                trimHist[trimHistPos] = v; trimHistPos = (trimHistPos + 1) & 7; trimHistN = std::min (trimHistN + 1, 8);
            }
        }
        if (trimHistN >= 3)
        {
            double t[8] = {}, dev[8] = {};
            for (int i = 0; i < trimHistN; ++i) t[i] = trimHist[i];
            const double med = medianOf (t, trimHistN);
            for (int i = 0; i < trimHistN; ++i) dev[i] = std::abs (t[i] - med);
            const double mad = medianOf (dev, trimHistN);
            trimAutoValid = mad <= 0.02 * med;
            if (trimAutoValid) trimAuto = med;
        }
        trimWinActive = false;
    }

    void onBassOnset()
    {
        curNote = rhythm.onBass (nowSamp);
        if (rhythm.getState() == RhythmTracker::Fill) clearMemory();
        noteStart = nowSamp;
        noteFastDone = noteHannDone = false;
    }

    // phase of the bass relative to the kick reference; false = no reference available
    bool measure (bool precise, double& deltaDeg, bool& live)
    {
        Cx ref; live = false;
        bool have = false;
        if (ghostOn && ghostSValid && ghostSAge < (int) (0.4 * fs))        { ref = ghostS; have = true; }
        else if (ghostOn && ghostPValid && (nowSamp - hitOnset) < (int64_t) (0.5 * fs)) { ref = ghostP; have = true; }
        else if (ghostOn && ghostWValid && ghostWAge < (int) (0.4 * fs))   { ref = ghostW; have = true; }
        else if (kGate)                                                    { ref = { sKc, -sKs }; have = true; live = true; }
        if (! have) return false;

        Cx zb; double amp;
        if (precise)
        {
            const double ang = 2.0 * kPi * (double) hpos / (double) M;     // hpos = start of the window
            const Cx e1 { std::cos (ang), -std::sin (ang) };               // e^{-j ang}
            const Cx e3 { std::cos (ang),  std::sin (ang) };               // e^{+j ang}
            const Cx t1 = cmul (e1, { Yr[0], Yi[0] }), t3 = cmul (e3, { Yr[2], Yi[2] });
            zb = { 0.5 * Yr[1] - 0.25 * t1.re - 0.25 * t3.re, 0.5 * Yi[1] - 0.25 * t1.im - 0.25 * t3.im };
            amp = 4.0 * std::hypot (zb.re, zb.im) / (double) M;
        }
        else
        {
            zb = { sBc, -sBs };
            amp = 2.0 * std::hypot (sBc, sBs) / (double) N;
        }
        if (amp < 0.3 * thrOn) return false;
        const Cx d = cmulc (zb, ref);
        deltaDeg = std::atan2 (d.im, d.re) * 180.0 / kPi;
        return true;
    }

    void schedule (bool have, double deltaDeg, bool precise, bool live)
    {
        pendValid = true;
        pendIdentity = ! have;
        pendDelta = deltaDeg; pendPrecise = precise; pendLive = live;
        pendSlot = curNote.slot;
        pendConfident = false;
        pendStable = curNote.valid && curNote.onGrid && curNote.expected
                     && rhythm.getState() != RhythmTracker::Fill;
        pendAt = noteStart + (int64_t) applyOffset;
    }

    void applyPending()
    {
        pendValid = false;
        if (pendIdentity)
        {
            lagTarget = 0.0; polTarget = 1.0;
            meters.appliedDeg.store (0.f); meters.locked.store (0); meters.memoryUsed.store (0);
            return;
        }
        double delta = pendDelta; bool usedMem = false;
        if (stabOn && pendStable)
        {
            SlotMem& m = mem[(size_t) (pendSlot & 3)];
            if (memoryConsistent (m))
            {
                const double memDeg = std::atan2 (m.im, m.re) * 180.0 / kPi;
                const bool far = std::abs (wrap180 (pendDelta - memDeg)) > 30.0;
                if (far && m.out == 0)            { m.out = 1; delta = memDeg; usedMem = true; }   // first outlier: treat as a glitch
                else if (far)                     { m = SlotMem(); memoryLearn (pendDelta, pendSlot); }   // repeated: a real change
                else if (! pendConfident)         { delta = memDeg; usedMem = true; }              // doubtful but close: the history wins
                else                              { memoryLearn (pendDelta, pendSlot); }
            }
            else if (pendConfident) memoryLearn (pendDelta, pendSlot);
        }
        const double lagTot = wrap360 (delta - target);
        if (lagTot < 180.0) { polTarget =  1.0; lagTarget = lagTot; }
        else                { polTarget = -1.0; lagTarget = lagTot - 180.0; }
        meters.deltaDeg.store ((float) delta);
        meters.appliedDeg.store ((float) lagTot);
        meters.locked.store (pendPrecise ? 1 : 2);
        meters.memoryUsed.store (usedMem ? 1 : 0);
        meters.hits.fetch_add (1);
    }

    // ------------------------------------------------------------------ analysis
    void analyse (float kIn, float bIn, float mIn, bool haveKick)
    {
        ++nowSamp;

        // activity detectors: 200 Hz low-passed, fast attack / short release
        kLp1 += detLp * (kIn - kLp1); kLp2 += detLp * (kLp1 - kLp2);
        bLp1 += detLp * (bIn - bLp1); bLp2 += detLp * (bLp1 - bLp2);
        mLp1 += detLp * (mIn - mLp1); mLp2 += detLp * (mLp1 - mLp2);
        // quadrature amplitude: sqrt (x(t)^2 + x(t - T/4)^2) is ripple-free for a tone at f0
        const double kd = kq[(size_t) qpos], bd = bq[(size_t) qpos];
        kq[(size_t) qpos] = kLp2; bq[(size_t) qpos] = bLp2;
        if (++qpos >= (int) kq.size()) qpos = 0;
        const double ka = std::sqrt (kLp2 * kLp2 + kd * kd), ba = std::sqrt (bLp2 * bLp2 + bd * bd);
        kEnv += (ka > kEnv ? envAtk : envRel) * (ka - kEnv);
        bEnv += (ba > bEnv ? envAtk : envRel) * (ba - bEnv);

        // kick present (absolute threshold with hysteresis) and kick ONSET (relative detector)
        kGate = haveKick && (kGate ? kEnv > thrOff : kEnv > thrOn);
        if (haveKick)
        {
            if (kNote.update (kEnv, thrOn, thrOff) > 0) onKickOnset();
        }
        else kNote = NoteGate();

        // auto-trim statistics (fixed window after each kick onset)
        if (trimWinActive)
        {
            const int64_t age = nowSamp - trimWinStart;
            if (age >= trimBegin && age < trimEnd) { trimNum += mLp2 * kLp2; trimDen += kLp2 * kLp2; }
            else if (age >= trimEnd) finishTrimWindow();
        }

        // bass note detector. Closes when the level drops to 20 % of the note's peak
        // (so 10 ms gaps between 16th notes are seen), re-opens only on a real rise.
        if (bGate)
        {
            bPeak = std::max (bPeak, bEnv);
            if (bEnv < std::max (thrOff, 0.2 * bPeak))
            {
                bGate = false; bMin = bEnv; bAge = 0; meters.locked.store (0);
            }
            else if (bAge < 1000000000) ++bAge;
        }
        else
        {
            bMin = std::min (bMin, bEnv);
            if (bEnv > thrOn && bEnv > 2.0 * bMin)
            {
                bGate = true; bPeak = bEnv; bAge = 0;
                onBassOnset();
            }
        }

        // one-period sliding demodulator for kick (and the fast bass estimate)
        const float c = cosT[(size_t) idx], s = sinT[(size_t) idx];
        float* r = ring.data() + (size_t) idx * 4;
        const float pkc = kIn * c, pks = kIn * s, pbc = bIn * c, pbs = bIn * s;
        sKc += (double) pkc - (double) r[0];  r[0] = pkc;
        sKs += (double) pks - (double) r[1];  r[1] = pks;
        sBc += (double) pbc - (double) r[2];  r[2] = pbc;
        sBs += (double) pbs - (double) r[3];  r[3] = pbs;
        if (++idx >= N) idx = 0;

        // Hann-weighted 2-period demodulator for the bass (3 bins of a 2N DFT)
        {
            const float xo = bRing[(size_t) hpos];
            bRing[(size_t) hpos] = bIn;
            const double dlt = (double) bIn - (double) xo;
            for (int q = 0; q < 3; ++q)
            {
                Yr[q] += dlt * (double) twC[q][(size_t) hpos];
                Yi[q] -= dlt * (double) twS[q][(size_t) hpos];
            }
            if (++hpos >= M) hpos = 0;
        }

        // keep the running sums honest: after a long silence everything must be exactly zero
        if (std::abs (kIn) < 1.0e-7f && std::abs (bIn) < 1.0e-7f)
        {
            if (++silentRun == M + 1)
            {
                sKc = sKs = sBc = sBs = 0.0;
                for (int q = 0; q < 3; ++q) Yr[q] = Yi[q] = 0.0;
            }
        }
        else silentRun = 0;
        if (! std::isfinite (Yr[1] + Yi[1] + sKc + sBc)) { sKc = sKs = sBc = sBs = 0.0; for (int q = 0; q < 3; ++q) Yr[q] = Yi[q] = 0.0; }

        // ---- kick reference: a "settled" window = frequency equals the focus frequency -----
        if (ghostSAge < (1 << 30)) ++ghostSAge;
        if (ghostWAge < (1 << 30)) ++ghostWAge;
        if (++stabCount >= stabStep)
        {
            stabCount = 0;
            const Cx zk { sKc, -sKs };
            if (hasPrevZk)
            {
                const Cx d = cmulc (zk, prevZk);
                const double fOff = std::atan2 (d.im, d.re) * fs / (2.0 * kPi * (double) stabStep);   // Hz away from f0
                const double ampK = 2.0 * std::hypot (zk.re, zk.im) / (double) N;
                if (haveKick && kGate && ampK > 3.0 * thrOn)
                {
                    if (std::abs (fOff) < 0.6) ++stableRun; else stableRun = 0;
                    if (stableRun >= 4)                    // frequency == focus for ~2.5 periods: the pitch sweep is over
                    {
                        hitPeak = std::max (hitPeak, ampK);
                        if (ampK >= 0.4 * hitPeak)
                        {
                            ghostS = zk; ghostSAge = 0; ghostSValid = true;
                            lastGood = zk; lastGoodOnset = hitOnset; lastGoodValid = true;
                        }
                    }
                    if (ampK > 6.0 * thrOn) { ghostW = zk; ghostWAge = 0; ghostWValid = true; }   // fallback: strongest recent window
                }
            }
            prevZk = zk; hasPrevZk = true;
        }

        // ---- estimates for the current bass note ---------------------------------------------
        if (autoActive && bGate)
        {
            if (! noteFastDone && bAge >= needFast)
            {
                noteFastDone = true;
                double dl = 0.0; bool live = false;
                const bool have = measure (false, dl, live);
                fastValid = have; fastDelta = dl;
                schedule (have, dl, false, live);          // replaced by the precise estimate if the note lasts long enough
            }
            if (hannOk && ! noteHannDone && bAge >= needHann)
            {
                noteHannDone = true;
                double dl = 0.0; bool live = false;
                if (measure (true, dl, live))
                {
                    schedule (true, dl, true, live);
                    pendConfident = fastValid && std::abs (wrap180 (dl - fastDelta)) < 15.0;
                }
            }
        }

        if (pendValid && nowSamp >= pendAt) applyPending();
    }

    // ------------------------------------------------------------------ state
    double fs = 0.0, f0 = 55.0, tanF0 = 0.0;
    int    latency = 0, leadSamples = 0, wp = 0, N = 0, M = 0, idx = 0, hpos = 0;
    std::vector<float> dX[2], dK[2], cosT, sinT, ring, bRing, twC[3], twS[3];
    std::vector<double> kq, bq;
    int    qpos = 0, silentRun = 0, stabStep = 100, stabCount = 0, stableRun = 0;
    double sKc = 0, sKs = 0, sBc = 0, sBs = 0, Yr[3] = {}, Yi[3] = {};
    double kLp1 = 0, kLp2 = 0, bLp1 = 0, bLp2 = 0, mLp1 = 0, mLp2 = 0, kEnv = 0, bEnv = 0;
    double detLp = 0, envAtk = 0, envRel = 0, polCoef = 0, lagCoef = 0, wetCoef = 0, trimCoef = 0;
    double thrOn = 0.005, thrOff = 0.0025, target = 0.0;
    bool   kGate = false, bGate = false, autoActive = true, ghostOn = true, stabOn = true,
           autoTrimOn = true, group = true, hannOk = true;
    int    bAge = 0, settleSamples = 0, needFast = 0, needHann = 0, applyOffset = 0;
    double bPeak = 0.0, bMin = 0.0;
    NoteGate kNote;
    // kick reference
    Cx     ghostS, ghostW, ghostP, lastGood, prevZk;      // settled / strongest / predicted / last settled phasor
    int    ghostSAge = 1 << 30, ghostWAge = 1 << 30;
    bool   ghostSValid = false, ghostWValid = false, ghostPValid = false, lastGoodValid = false;
    int64_t hitOnset = 0, lastGoodOnset = 0;
    double hitPeak = 0.0;
    bool   hasPrevZk = false;
    // pending correction
    bool   pendValid = false, pendIdentity = false, pendPrecise = false, pendLive = false, pendStable = false;
    bool   noteFastDone = false, noteHannDone = false, fastValid = false, pendConfident = false;
    double pendDelta = 0.0, fastDelta = 0.0;
    int    pendSlot = 0;
    int64_t pendAt = 0, noteStart = 0, nowSamp = 0;
    // control
    double lagTarget = 0, polTarget = 1, lagSm = 0, polSm = 1, lagUsed = -1, wetSm = 1;
    double trimSm = 0.0, trimAuto = 1.0, trimNum = 0.0, trimDen = 0.0, trimHist[8] = {};
    bool   trimAutoValid = false, trimWinActive = false;
    int    trimHistN = 0, trimHistPos = 0;
    int64_t trimWinStart = 0, trimBegin = 0, trimEnd = 0;
    int    ctl = 0;
    double a1 = 1, a2 = 0, a3 = 0, k = 1.4142135623730951;
    double ic1[2] { 0, 0 }, ic2[2] { 0, 0 };
    // groove
    RhythmTracker rhythm;
    RhythmTracker::NoteInfo curNote;
    SlotMem mem[4];
};

} // namespace psy
