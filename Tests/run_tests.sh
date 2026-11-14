#!/usr/bin/env bash
# Builds and runs every test of the DSP engine (needs g++ and python3 with numpy).  Run from this folder.
set -e
T=$(mktemp -d)
CXX="g++ -O2 -std=c++17 -Wall -Wextra"
echo "== 1. all-pass phase accuracy";           $CXX test_core.cpp -o $T/core && $T/core --sweep 44100 55 | tail -1 && $T/core --sweep 48000 70 | tail -1
echo "== 2. groove detector (onset times)";      $CXX test_rhythm.cpp -o $T/rh && $T/rh
echo "== 3. robustness matrix";                  $CXX test_robust.cpp -o $T/rb && $T/rb
echo "== 4. CPU";                                $CXX bench2.cpp -o $T/bn && $T/bn
echo "== 5. scenarios, old engine vs new engine"
$CXX -DV2 -DCORE_HEADER='"../Source/PhaseAlignCore.h"' test_scen.cpp -o $T/scen_v2
$CXX -DCORE_HEADER='"PhaseAlignCore_v1_legacy.h"' test_scen.cpp -o $T/scen_v1
SCEN_V1=$T/scen_v1 SCEN_V2=$T/scen_v2 python3 eval_scen.py
rm -rf $T
