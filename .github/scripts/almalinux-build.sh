#!/usr/bin/env bash
# Run inside the prepared image. The source mount stays read-only.
set -euo pipefail
case ${CI_CHECK:?set CI_CHECK} in
  gcc|clang|gcc-san|clang-san|fuzz) ;;
  *) echo 'Unknown CI_CHECK.' >&2; exit 2 ;;
esac
mkdir -p /reports /tmp/libvsr-work
exec > >(tee /reports/build.log) 2>&1
# The GCC toolset supplies the C++23 standard library for both compilers.
source /opt/rh/gcc-toolset-14/enable
cat /etc/os-release
g++ --version
clang++ --version
cmake --version
rpm -qa | sort > /reports/packages.txt
tar -C /source --exclude=./.git --exclude=./build --exclude=./reports -cf - . |
  tar -C /tmp/libvsr-work --no-same-owner -xf -
cd /tmp/libvsr-work
trap 'cp -a build/*/Testing/Temporary/LastTest.log /reports/ 2>/dev/null || true' EXIT

preset=$CI_CHECK
compiler=g++
flags=()
if [[ $CI_CHECK == clang* || $CI_CHECK == fuzz ]]; then
  compiler=clang++
  gcc_install_dir=$(dirname "$(g++ -print-libgcc-file-name)")
  flags+=("-DCMAKE_CXX_FLAGS=--gcc-install-dir=$gcc_install_dir")
fi
if [[ $CI_CHECK == fuzz ]]; then
  preset=clang
  # Instrument the production decoder as well as the fuzz entry point.
  flags+=(-DVSR_BUILD_FUZZ=ON -DVSR_ENABLE_ASAN=ON -DVSR_ENABLE_UBSAN=ON)
fi
cmake --preset "$preset" -DCMAKE_CXX_COMPILER="$compiler" "${flags[@]}"
if [[ $CI_CHECK == fuzz ]]; then
  cmake --build --preset "$preset" --parallel 2 --target vsr_decode_fuzz
  ./build/clang/fuzz/vsr_decode_fuzz -max_total_time=60 -max_len=1048576
else
  cmake --build --preset "$preset" --parallel 2
  ctest --preset "$preset" --timeout 180
  simulator=./build/$preset/sim/vsr_sim
  bash /source/tools/test_sim_seeds.sh
  source /source/.github/scripts/sim-seeds.sh
  # Fixed set: the regression seeds. Fresh set: new seeds for each CI run.
  run_seeds "$simulator" 1 300 3
  run_seeds "$simulator" 1 300 5
  fresh=$(derive_first_seed "${CI_SEED_SOURCE:?set CI_SEED_SOURCE}:$CI_CHECK")
  echo "Fresh seed source: $CI_SEED_SOURCE:$CI_CHECK"
  run_seeds "$simulator" "$fresh" 200 3
  run_seeds "$simulator" "$fresh" 200 5
fi
