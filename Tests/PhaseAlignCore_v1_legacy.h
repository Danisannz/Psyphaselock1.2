// =============================================================================
//  PhaseAlignCore.h  -  Kick/Bass phase alignment engine (no JUCE dependency)
//
//  * Measures kick and bass phase at a "focus" frequency (bass root, 40-100 Hz)
//    with an exact one-period sliding demodulator (8 MACs / sample, no FFT).
//  * When kick AND bass are both active for one full period, the relative phase
//    is latched (once per event) and the bass is rotated by an all-pass (+
//    polarity flip) so bass and kick are in phase at the focus frequency.
//  * A fixed 30 ms look-ahead (reported as plugin latency) puts the correction
//    in place BEFORE the bass note starts.
//  * Pure all-pass rotation => flat magnitude, the bass is not filtered.
//
//  Track : main = bass only,  sidechain = kick.   out = aligned bass
//  Group : main = kick+bass,  sidechain = kick.   bass = main - kick;
//          out = main + wet * (rot(bass) - bass)   (everything else untouched)
// =============================================================================
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace psy
{
constexpr double kPi = 3.14159265358979323846;

struct Params
{
    bool  groupMode   = true;
    float focusHz     = 55.0f;    // 40..100
    bool  autoAlign   = true;
    float manualDeg   = 0.0f;     // 0..360 bass phase lag when auto is off
    float targetDeg   = 0.0f;     // -90..90 wanted bass phase relative to kick
    float thresholdDb = -45.0f;   // activity gate for kick / bass
    float wet         = 1.0f;     // 0..1
    float kickTrimDb  = 0.0f;     // group mode: level match of sidechain kick
    float settleMs    = 0.0f;     // 0..40 extra wait before measuring (kick pitch sweep)
    bool  ghostRef    = true;     // bass notes with no kick: follow the phase of the last kick
};

class PhaseAligner
{
public:
    static constexpr double kLatencySeconds = 0.030;
    static constexpr double kQ              = 0.7071067811865476;

    struct Meters
    {
        std::atomic<float>    deltaDeg   { 0.f };   // bass-vs-kick phase before correction
        std::atomic<float>    appliedDeg { 0.f };   // correction applied (0..360, incl. polarity)
        std::atomic<float>    kickDb     { -100.f };
        std::atomic<float>    bassDb     { -100.f };
        std::atomic<int>      locked     { 0 };
        std::atomic<uint32_t> hits       { 0 };     // +1 on every new latch
    } meters;

    // Optional per-sample taps for the scope (mono, same timeline = delayed by the look-ahead)
    struct Tap { float* kick = nullptr; float* bassIn = nullptr; float* bassOut = nullptr; };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        latency = (int) std::lround (kLatencySeconds * fs);
        for (int c = 0; c < 2; ++c)
        {
            dX[c].assign ((size_t) latency + 1, 0.f);
            dK[c].assign ((size_t) latency + 1, 0.f);
        }
        detLp   = 1.0 - std::exp (-2.0 * kPi * 200.0 / fs);
        envAtk  = 1.0 - std::exp (-1.0 / (0.0003 * fs));
        envRel  = 1.0 - std::exp (-1.0 / (0.0015 * fs));
        polCoef = 1.0 - std::exp (-1.0 / (0.0030 * fs));
        lagCoef = 1.0 - std::exp (-1.0 / (0.0060 * fs));
        wetCoef = 1.0 - std::exp (-1.0 / (0.0050 * fs));
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
        wp = 0;
        N = 0;                                   // forces analysis rebuild
        kLp1 = kLp2 = bLp1 = bLp2 = 0.0;
        kEnv = bEnv = 0.0; bPeak = bMin = 0.0; ghostAge = 1 << 30;
        kGate = bGate = false;
        kAge = bAge = 0;
        latched = false;
        lagTarget = 0.0; polTarget = 1.0;
        lagSm = 0.0; polSm = 1.0; lagUsed = -1.0; ctl = 0;
        wetSm = 1.0;
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
        const float trim = p.groupMode ? (float) dbToLin (p.kickTrimDb) : 0.0f;
        thrOn  = dbToLin (p.thresholdDb);
        thrOff = thrOn * 0.5;
        target = (double) p.targetDeg;
        settleSamples = (int) (std::clamp (p.settleMs, 0.0f, 40.0f) * 0.001 * fs);
        ghostOn = p.ghostRef;
        const double wetT = (double) std::clamp (p.wet, 0.0f, 1.0f);

        if (! autoActive)
        {
            const double m = wrap360 ((double) p.manualDeg);
            if (m < 180.0) { polTarget =  1.0; lagTarget = m; }
            else           { polTarget = -1.0; lagTarget = m - 180.0; }
            meters.appliedDeg.store ((float) m);
            meters.locked.store (0);
        }

        const size_t dsz = (size_t) latency + 1;

        for (int i = 0; i < n; ++i)
        {
            const float x0 = main[0][i];
            const float x1 = numCh > 1 ? main[1][i] : x0;
            float k0 = 0.f, k1 = 0.f;
            if (haveKick) { k0 = kick[0][i]; k1 = kickCh > 1 ? kick[1][i] : k0; }

            // analysis on the un-delayed signals
            const float km = 0.5f * (k0 + k1);
            const float bm = 0.5f * (x0 + x1) - trim * km;
            analyse (km, bm, haveKick);

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

        meters.kickDb.store ((float) linToDb (kEnv));
        meters.bassDb.store ((float) linToDb (bEnv));
    }

private:
    static double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
    static double linToDb (double x)  { return 20.0 * std::log10 (std::max (x, 1e-6)); }
    static double wrap360 (double d)  { d = std::fmod (d, 360.0); return d < 0.0 ? d + 360.0 : d; }

    void setupFocus (float focusHz)
    {
        focusHz = std::clamp (focusHz, 20.0f, 200.0f);
        const int n = std::max (16, (int) std::lround (fs / (double) focusHz));
        if (n == N) return;
        N = n;
        f0 = fs / (double) N;                    // one window = exactly one period
        cosT.resize ((size_t) N); sinT.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const double th = 2.0 * kPi * (double) i / (double) N;
            cosT[(size_t) i] = (float) std::cos (th);
            sinT[(size_t) i] = (float) std::sin (th);
        }
        ring.assign ((size_t) N * 4, 0.f);
        const int nq = std::max (2, N / 4);          // quarter period
        kq.assign ((size_t) nq, 0.0); bq.assign ((size_t) nq, 0.0); qpos = 0;
        kEnv = bEnv = 0.0;
        sKc = sKs = sBc = sBs = 0.0;
        idx = 0;
        latched = false; kAge = bAge = 0;
        tanF0 = std::tan (kPi * f0 / fs);
        lagUsed = -1.0;                          // force coefficient refresh
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

    void analyse (float kIn, float bIn, bool haveKick)
    {
        // activity detectors: 200 Hz low-passed, fast attack / short release
        kLp1 += detLp * (kIn - kLp1); kLp2 += detLp * (kLp1 - kLp2);
        bLp1 += detLp * (bIn - bLp1); bLp2 += detLp * (bLp1 - bLp2);
        // quadrature amplitude: sqrt (x(t)^2 + x(t - T/4)^2) is ripple-free for a tone at f0
        const double kd = kq[(size_t) qpos], bd = bq[(size_t) qpos];
        kq[(size_t) qpos] = kLp2; bq[(size_t) qpos] = bLp2;
        if (++qpos >= (int) kq.size()) qpos = 0;
        const double ka = std::sqrt (kLp2 * kLp2 + kd * kd), ba = std::sqrt (bLp2 * bLp2 + bd * bd);
        kEnv += (ka > kEnv ? envAtk : envRel) * (ka - kEnv);
        bEnv += (ba > bEnv ? envAtk : envRel) * (ba - bEnv);

        // kick: absolute threshold with hysteresis ("kick is present")
        const bool kOn = haveKick && (kGate ? kEnv > thrOff : kEnv > thrOn);
        if (kOn != kGate) { kGate = kOn; kAge = 0; if (kOn) latched = false; }   // only a kick ONSET re-arms; the kick ending must not undo a correction
        else if (kOn && kAge < 1000000000) ++kAge;

        // bass: note detector. Closes when the level drops to 20 % of the note's peak
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
                bGate = true; bPeak = bEnv; bAge = 0; latched = false; meters.locked.store (0);
            }
        }

        // sliding one-period demodulator (exact null at DC and 2*f0)
        const float c = cosT[(size_t) idx], s = sinT[(size_t) idx];
        float* r = ring.data() + (size_t) idx * 4;
        const float pkc = kIn * c, pks = kIn * s, pbc = bIn * c, pbs = bIn * s;
        sKc += (double) pkc - (double) r[0];  r[0] = pkc;
        sKs += (double) pks - (double) r[1];  r[1] = pks;
        sBc += (double) pbc - (double) r[2];  r[2] = pbc;
        sBs += (double) pbs - (double) r[3];  r[3] = pbs;
        if (++idx >= N) idx = 0;

        // remember the phase of the kick while it is strong ("ghost kick" reference)
        if (ghostAge < (1 << 30)) ++ghostAge;
        if (haveKick && kGate)
        {
            const double a2 = (2.0 / (double) N) * (2.0 / (double) N) * (sKc * sKc + sKs * sKs);
            if (a2 > 36.0 * thrOn * thrOn) { ghostRe = sKc; ghostIm = sKs; ghostAge = 0; }
        }

        // bass note with no kick underneath: nothing to align -> leave the bass untouched.
        // (decided at the same moment a latch would happen, so the change lands in the gap before the note)
        if (autoActive && bGate && ! kGate && bAge == N + (int) (0.0007 * fs) + settleSamples)   // one-shot per note
        {
            const double ampB = 2.0 * std::hypot (sBc, sBs) / (double) N;
            if (ghostOn && ghostAge < (int) (0.4 * fs) && ampB > 0.3 * thrOn)
            {
                // follow the continuation of the last kick's phase (same maths as a real latch)
                const double re = sBc * ghostRe + sBs * ghostIm;
                const double im = sBc * ghostIm - sBs * ghostRe;
                const double delta  = std::atan2 (im, re) * 180.0 / kPi;
                const double lagTot = wrap360 (delta - target);
                if (lagTot < 180.0) { polTarget =  1.0; lagTarget = lagTot; }
                else                { polTarget = -1.0; lagTarget = lagTot - 180.0; }
                meters.deltaDeg.store ((float) delta);
                meters.appliedDeg.store ((float) lagTot);
                meters.locked.store (2);                       // 2 = locked to the ghost kick
                meters.hits.fetch_add (1);
            }
            else
            {
                lagTarget = 0.0; polTarget = 1.0;
                meters.appliedDeg.store (0.f); meters.locked.store (0);
            }
            latched = true;
        }

        // latch once both sources have been active for a full window
        const int need = N + (int) (0.0007 * fs) + settleSamples;
        if (autoActive && kGate && bGate && ! latched && kAge >= need && bAge >= need)
        {
            const double ampK = 2.0 * std::hypot (sKc, sKs) / (double) N;
            const double ampB = 2.0 * std::hypot (sBc, sBs) / (double) N;
            if (ampK > 0.3 * thrOn && ampB > 0.3 * thrOn)
            {
                // Zk = (sKc, -sKs), Zb = (sBc, -sBs); delta = arg (Zb * conj (Zk)) = phiB - phiK
                const double re = sBc * sKc + sBs * sKs;
                const double im = sBc * sKs - sBs * sKc;
                const double delta  = std::atan2 (im, re) * 180.0 / kPi;
                const double lagTot = wrap360 (delta - target);
                if (lagTot < 180.0) { polTarget =  1.0; lagTarget = lagTot; }
                else                { polTarget = -1.0; lagTarget = lagTot - 180.0; }
                latched = true;
                meters.deltaDeg.store ((float) delta);
                meters.appliedDeg.store ((float) lagTot);
                meters.locked.store (1);
                meters.hits.fetch_add (1);
            }
        }
    }

    double fs = 0.0, f0 = 55.0, tanF0 = 0.0;
    int    latency = 0, wp = 0, N = 0, idx = 0;
    std::vector<float> dX[2], dK[2], cosT, sinT, ring;
    std::vector<double> kq, bq;
    int    qpos = 0, settleSamples = 0;
    double bPeak = 0.0, bMin = 0.0;
    double ghostRe = 0.0, ghostIm = 0.0;     // last strong kick phasor (oscillator-referenced)
    int    ghostAge = 1 << 30;
    bool   ghostOn = true;
    double sKc = 0, sKs = 0, sBc = 0, sBs = 0;
    double kLp1 = 0, kLp2 = 0, bLp1 = 0, bLp2 = 0, kEnv = 0, bEnv = 0;
    double detLp = 0, envAtk = 0, envRel = 0, polCoef = 0, lagCoef = 0, wetCoef = 0;
    double thrOn = 0.005, thrOff = 0.0025, target = 0.0;
    bool   kGate = false, bGate = false, latched = false, autoActive = true;
    int    kAge = 0, bAge = 0;
    double lagTarget = 0, polTarget = 1, lagSm = 0, polSm = 1, lagUsed = -1, wetSm = 1;
    int    ctl = 0;
    double a1 = 1, a2 = 0, a3 = 0, k = 1.4142135623730951;
    double ic1[2] { 0, 0 }, ic2[2] { 0, 0 };
};

} // namespace psy
