#!/usr/bin/env bash
# Helpers to source. They run vsr_sim and print a replay command on failure.

# Print a first seed from the words in "$1". Each new word gives a new seed.
# The seed has 48 bits. It stays above 1000, so it never overlaps seeds 1..300.
derive_first_seed() {
  local hex
  hex=$(printf '%s' "$1" | sha256sum | cut -c1-12)
  echo $((16#$hex + 1000))
}

# run_seeds <simulator> <first seed> <seed count> <replicas> [simulator options]
# Print the range and the result. On failure, print the command to replay the
# failed seed. If the seed is not known, print the command for the whole range.
run_seeds() {
  local simulator=$1 first=$2 count=$3 replicas=$4
  shift 4
  echo "SEEDS first=$first count=$count last=$((first + count - 1)) replicas=$replicas $*"
  local output status=0
  output=$("$simulator" "$first" "$count" "$replicas" "$@") || status=$?
  echo "$output"
  if ((status != 0)); then
    local failed
    failed=$(sed -n 's/^FAIL seed=\([0-9]*\).*/\1/p' <<<"$output" | head -n 1)
    # An assertion abort or a sanitizer stop prints no FAIL line. The run is
    # deterministic, so run the range again one seed at a time to find the seed.
    [[ -n $failed ]] || failed=$(find_failed_seed "$simulator" "$first" "$count" "$replicas" "$@")
    if [[ -n $failed ]]; then
      echo "REPLAY failed seed: $simulator $failed 1 $replicas $*" >&2
    else
      echo "REPLAY failed range, the seed is not known: $simulator $first $count $replicas $*" >&2
    fi
    return 1
  fi
}

# find_failed_seed <simulator> <first seed> <seed count> <replicas> [simulator options]
# Print the first seed that makes the simulator exit with an error.
# Print nothing if every seed passes alone.
find_failed_seed() {
  local simulator=$1 first=$2 count=$3 replicas=$4 seed
  shift 4
  for ((seed = first; seed < first + count; ++seed)); do
    if ! "$simulator" "$seed" 1 "$replicas" "$@" >/dev/null; then
      echo "$seed"
      return 0
    fi
  done
}
