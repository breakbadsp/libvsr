#!/usr/bin/env bash
# Run inside the prepared image. The source mount stays read-only.
# Step 1: the mutation check. Step 2: a soak of fresh seeds on parallel shards.
# Inputs, all from the environment:
#   SOAK_SEED_SOURCE     words that give the first seed
#   SOAK_FIRST_SEED      optional first seed, replaces the derived one
#   SOAK_SEEDS_PER_SHARD optional seed limit for each shard, 0 means no limit
#   SOAK_BUDGET_MINUTES  total time for this script
set -euo pipefail
start=$SECONDS
budget=$((${SOAK_BUDGET_MINUTES:?set SOAK_BUDGET_MINUTES} * 60))
limit=${SOAK_SEEDS_PER_SHARD:-0}
for value in "$budget" "$limit" "${SOAK_FIRST_SEED:-0}"; do
  [[ $value =~ ^[0-9]+$ ]] || { echo 'Seed and time inputs must be numbers.' >&2; exit 2; }
done
mkdir -p /reports /tmp/libvsr-work
rm -f /tmp/libvsr-soak-stop
exec > >(tee /reports/soak.log) 2>&1
source /opt/rh/gcc-toolset-14/enable
tar -C /source --exclude=./.git --exclude=./build --exclude=./reports -cf - . |
  tar -C /tmp/libvsr-work --no-same-owner -xf -
cd /tmp/libvsr-work
source .github/scripts/sim-seeds.sh

cmake --preset gcc -DCMAKE_CXX_COMPILER=g++
# Limit the builds that the mutation check starts. The host has few CPUs.
export CMAKE_BUILD_PARALLEL_LEVEL=2
cmake --build --preset gcc

echo "== Mutation check =="
# The check edits and restores files in this writable copy only.
python3 tools/mutation_check.py 2>&1 | tee /reports/mutation.log

first=${SOAK_FIRST_SEED:-}
[[ -n $first && $first != 0 ]] || first=$(derive_first_seed "${SOAK_SEED_SOURCE:?set SOAK_SEED_SOURCE}")
echo "== Soak: first seed $first, seed source ${SOAK_SEED_SOURCE:-none} =="
simulator=./build/gcc/sim/vsr_sim
deadline=$((start + budget))
chunk=1000

# soak_shard <shard> <replica counts...>
# A shard has its own range of 2^32 seeds. It runs chunks of seeds, and
# takes the replica counts in turn, until the limit or the deadline.
soak_shard() {
  local shard=$1
  shift
  local next=$((first + shard * 4294967296)) done_count=0 index=0 replicas
  while ((SECONDS < deadline)) && { ((limit == 0)) || ((done_count < limit)); }; do
    local counts=("$@")
    replicas=${counts[index % ${#counts[@]}]}
    local count=$chunk
    ((limit != 0 && limit - done_count < count)) && count=$((limit - done_count))
    [[ ! -e /tmp/libvsr-soak-stop ]] || return 0
    run_seeds "$simulator" "$next" "$count" "$replicas" || { touch /tmp/libvsr-soak-stop; return 1; }
    next=$((next + count))
    done_count=$((done_count + count))
    index=$((index + 1))
  done
  echo "SHARD $shard done: $done_count seeds"
}

# Four shards: two for 3 replicas, one for 5, one for 1, 2, and 7.
pids=()
soak_shard 0 3 >/reports/shard-0.log 2>&1 & pids+=($!)
soak_shard 1 3 >/reports/shard-1.log 2>&1 & pids+=($!)
soak_shard 2 5 >/reports/shard-2.log 2>&1 & pids+=($!)
soak_shard 3 1 2 7 >/reports/shard-3.log 2>&1 & pids+=($!)
failed=0
for pid in "${pids[@]}"; do
  wait "$pid" || failed=1
done
for log in /reports/shard-*.log; do
  echo "-- $log: $(grep -c '^PASS' "$log") passed ranges"
  grep -E '^(FAIL|REPLAY)' "$log" || true
done
((failed == 0)) || { echo 'SOAK FAILED. Use the REPLAY commands above.' >&2; exit 1; }
echo 'SOAK PASSED'
