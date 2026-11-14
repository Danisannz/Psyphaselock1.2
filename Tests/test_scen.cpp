// Runs the core on /tmp/sc_main.f32 + /tmp/sc_side.f32.  Build with -DCORE_HEADER='"path"' and -DV2 for the new engine.
#include CORE_HEADER
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
static std::vector<float> rd (const char* f){ FILE* fp=fopen(f,"rb"); if(!fp){perror(f);exit(1);} fseek(fp,0,SEEK_END); long n=ftell(fp)/4; fseek(fp,0,SEEK_SET); std::vector<float> v((size_t)n); if(fread(v.data(),4,(size_t)n,fp)!=(size_t)n) exit(2); fclose(fp); return v; }
int main (int argc, char** argv)
{
    // args: out.f32 group(0/1) focus fs stabilize autotrim trimDb [hostBpm]
    const double fs = atof (argv[4]);
    psy::Params p; p.groupMode = atoi (argv[2]) != 0; p.focusHz = (float) atof (argv[3]); p.kickTrimDb = (float) atof (argv[7]);
#ifdef V2
    p.stabilize = atoi (argv[5]) != 0; p.autoTrim = atoi (argv[6]) != 0; p.hostBpm = argc > 8 ? (float) atof (argv[8]) : 0.f;
#endif
    auto m = rd ("/tmp/sc_main.f32"); auto k = rd ("/tmp/sc_side.f32"); const size_t n = std::min (m.size(), k.size());
    psy::PhaseAligner a; a.prepare (fs);
    std::mt19937 rng (9); std::uniform_int_distribution<int> bs (31, 1100);
    FILE* lg = fopen ("/tmp/sc_log.txt", "w");
    std::vector<float> m2 (m), k2 (k);
    for (size_t pos = 0; pos < n;)
    {
        const int len = (int) std::min<size_t> ((size_t) bs (rng), n - pos);
        float* mp[2] = { m.data() + pos, m2.data() + pos }; const float* kp[2] = { k.data() + pos, k2.data() + pos };
        a.process (mp, 2, kp, 2, len, p);
        pos += (size_t) len;
#ifdef V2
        fprintf (lg, "%zu %d %d %d %d %d %.2f %.2f\n", pos, a.meters.rhythmState.load(), a.meters.sixteenthPct.load(), a.meters.densityPct.load(), a.meters.onGridPct.load(), a.meters.memoryUsed.load(), a.meters.bpm.load(), a.meters.trimDb.load());
#endif
    }
    fclose (lg);
    FILE* o = fopen (argv[1], "wb"); fwrite (m.data(), 4, n, o); fclose (o);
    printf ("LAT %d\n", a.getLatencySamples());
}
