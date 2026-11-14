#include "../Source/PhaseAlignCore.h"
#include <chrono>
#include <cstdio>
#include <vector>
int main(){
  for (double fs : { 44100.0, 48000.0, 96000.0 }) for (int block : { 64, 512 }) {
    const int secs=60; psy::PhaseAligner a; a.prepare(fs); psy::Params p; std::vector<float> l(block),r(block),kl(block),kr(block);
    auto t0=std::chrono::steady_clock::now(); long samples=0;
    for(long b=0;b<(long)(fs*secs/block);++b){
      for(int i=0;i<block;++i){ double t=(double)(samples+i)/fs, ph=std::fmod(t,0.343);
        float k=(float)(0.8*std::sin(2*psy::kPi*(55+150*std::exp(-ph/0.012))*ph)*std::exp(-ph/0.08)*(ph<0.2));
        float bs=(float)(0.4*std::sin(2*psy::kPi*55*t+1.0)*(std::fmod(t,0.0857)<0.075)*(ph>0.08));
        l[i]=r[i]=bs+k; kl[i]=kr[i]=k; }
      samples+=block; float* m[2]={l.data(),r.data()}; const float* k[2]={kl.data(),kr.data()}; a.process(m,2,k,2,block,p);
    }
    double el=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    printf("fs %6.0f block %3d: %d s stereo -> %.3f s = %.2f%% of ONE core (%.0fx realtime)\n",fs,block,secs,el,100.0*el/secs,secs/el);
  }
}
