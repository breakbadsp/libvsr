#!/usr/bin/env bash
# Test the REPLAY line of run_seeds with fake simulators.
# Usage: tools/test_sim_seeds.sh
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../.github/scripts/sim-seeds.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# A fake simulator. Seed $BAD_SEED fails in the mode that FAKE_MODE names:
# "fail" prints a FAIL line, "abort" aborts with no FAIL line, and
# "pass" passes every seed.
cat >"$work/fake_sim" <<'SIM'
#!/usr/bin/env bash
first=$1 count=$2
for ((seed = first; seed < first + count; ++seed)); do
  if [[ $seed == "${BAD_SEED:-}" && $FAKE_MODE == fail ]]; then
    echo "FAIL seed=$seed replicas=$3: broken"
    exit 1
  fi
  if [[ $seed == "${BAD_SEED:-}" && $FAKE_MODE == abort ]]; then
    echo "assertion failed" >&2
    kill -ABRT $$
  fi
done
echo "PASS seeds=$first..$((first + count - 1))"
SIM
chmod +x "$work/fake_sim"

check() { # <name> <expected REPLAY line> <mode> <bad seed>
  local name=$1 expected=$2 replay
  replay=$(FAKE_MODE=$3 BAD_SEED=$4 run_seeds "$work/fake_sim" 100 50 3 2>&1 >/dev/null | grep '^REPLAY' || true)
  replay=${replay% }
  expected=${expected//@SIM@/$work/fake_sim}
  [[ $replay == "$expected" ]] || { echo "FAIL $name: got '$replay', want '$expected'" >&2; exit 1; }
  echo "ok $name"
}

check 'FAIL line' 'REPLAY failed seed: @SIM@ 130 1 3' fail 130
check 'abort in the middle' 'REPLAY failed seed: @SIM@ 130 1 3' abort 130
check 'abort at the first seed' 'REPLAY failed seed: @SIM@ 100 1 3' abort 100
check 'abort at the last seed' 'REPLAY failed seed: @SIM@ 149 1 3' abort 149
if FAKE_MODE=pass run_seeds "$work/fake_sim" 100 50 3 >/dev/null 2>&1; then echo 'ok pass'; else echo 'FAIL pass' >&2; exit 1; fi

# A failure that one seed alone does not show: replay the whole range.
cat >"$work/range_sim" <<'SIM'
#!/usr/bin/env bash
(($2 > 1)) && exit 3
echo "PASS"
SIM
chmod +x "$work/range_sim"
replay=$(run_seeds "$work/range_sim" 100 50 3 2>&1 >/dev/null | grep '^REPLAY' || true)
replay=${replay% }
  replay=${replay% }
[[ $replay == "REPLAY failed range, the seed is not known: $work/range_sim 100 50 3" ]] ||
  { echo "FAIL unknown seed: got '$replay'" >&2; exit 1; }
echo 'ok unknown seed'
