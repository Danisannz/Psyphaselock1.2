// Offline harness for the real C++ engine. Usage:
//   test_core <kick.f32> <bass.f32> <out_track.f32> <out_group.f32> <focus> <fs> [target]
//   test_core --sweep <fs> <focus>      (all-pass phase accuracy test)
#include "../Source/PhaseAlignCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <complex>

static std::vector<float> readF32 (const char* f)
{
    FILE* fp = fopen (f, "rb"); if (!fp) { perror (f); exit (1); }
    fseek (fp, 0, SEEK_END); long n = ftell (fp) / 4; fseek (fp, 0, SEEK_SET);
    std::vector<float> v ((size_t) n); if (fread (v.data(), 4, (size_t) n, fp) != (size_t) n) exit (2); fclose (fp); return v;
}
static void writeF32 (const char* f, const std::vector<float>& v)
{ FILE* fp = fopen (f, "wb"); fwrite (v.data(), 4, v.size(), fp); fclose (fp); }

static void run (psy::PhaseAligner& a, const psy::Params& p, std::vector<float>& main, const std::vector<float>& kick)
{
    const int block = 477;                      // odd block size on purpose
    std::vector<float> m2 (main), k2 (kick);
    for (size_t pos = 0; pos < main.size(); pos += (size_t) block)
    {
        int n = (int) std::min<size_t> ((size_t) block, main.size() - pos);
        float* mp[2] = { main.data() + pos, m2.data() + pos };
        const float* kp[2] = { kick.data() + pos, k2.data() + pos };
        a.process (mp, 2, kp, 2, n, p);
    }
}

int main (int argc, char** argv)
{
    if (argc >= 2 && ! strcmp (argv[1], "--sweep"))
    {
        double fs = atof (argv[2]); float focus = (float) atof (argv[3]);
        int bad = 0;
        for (double manual : { 0.0, 10.0, 45.0, 90.0, 135.0, 170.0, 180.0, 200.0, 270.0, 350.0 })
        {
            psy::PhaseAligner a; a.prepare (fs);
            psy::Params p; p.autoAlign = false; p.manualDeg = (float) manual; p.focusHz = focus; p.groupMode = false;
            int N = (int) std::lround (fs / focus); double f0 = fs / N;
            int len = (int) (fs * 1.5);
            std::vector<float> x ((size_t) len), kk ((size_t) len, 0.f);
            for (int i = 0; i < len; ++i) x[(size_t) i] = (float) (0.5 * std::cos (2 * psy::kPi * f0 * i / fs));
            std::vector<float> in (x);
            run (a, p, x, kk);
            // measure the phase of out relative to in delayed by latency, in the last second
            int L = a.getLatencySamples(); std::complex<double> zi = 0, zo = 0;
            for (int i = (int) fs; i < len; ++i)
            {
                double w = 2 * psy::kPi * f0 * i / fs;
                std::complex<double> e = std::polar (1.0, -w);
                zi += (double) in[(size_t) (i - L)] * e; zo += (double) x[(size_t) i] * e;
            }
            double lag = -std::arg (zo / zi) * 180 / psy::kPi; if (lag < 0) lag += 360;
            double err = std::fmod (lag - manual + 540.0, 360.0) - 180.0;
            double mag = std::abs (zo / zi);
            printf ("manual %6.1f deg -> measured lag %7.2f deg  (err %+.2f)  |gain| %.4f\n", manual, lag, err, mag);
            if (std::abs (err) > 1.0 || std::abs (mag - 1.0) > 0.01) ++bad;
        }
        printf (bad ? "SWEEP FAIL\n" : "SWEEP OK\n");
        return bad ? 1 : 0;
    }

    auto kick = readF32 (argv[1]); auto bass = readF32 (argv[2]);
    float focus = (float) atof (argv[5]); double fs = atof (argv[6]);
    float tgt = argc > 7 ? (float) atof (argv[7]) : 0.f;
    float settle = argc > 8 ? (float) atof (argv[8]) : 0.f;
    size_t n = std::min (kick.size(), bass.size());
    std::vector<float> trackOut (bass.begin(), bass.begin() + (long) n), groupOut (n);
    std::vector<float> k (kick.begin(), kick.begin() + (long) n);
    for (size_t i = 0; i < n; ++i) groupOut[i] = k[i] + bass[i];

    psy::Params p; p.focusHz = focus; p.targetDeg = tgt; p.settleMs = settle;
    { psy::PhaseAligner a; a.prepare (fs); p.groupMode = false; run (a, p, trackOut, k);
      printf ("track: latency %d samples, latches %u\n", a.getLatencySamples(), a.meters.hits.load()); }
    { psy::PhaseAligner a; a.prepare (fs); p.groupMode = true;  run (a, p, groupOut, k);
      printf ("group: latency %d samples, latches %u\n", a.getLatencySamples(), a.meters.hits.load()); }
    writeF32 (argv[3], trackOut); writeF32 (argv[4], groupOut);
    return 0;
}
