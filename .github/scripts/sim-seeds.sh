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
# Print the range and the result. On failure, print the command to replay it.
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
    echo "REPLAY failed seed: $simulator ${failed:-$first} 1 $replicas $*" >&2
    return 1
  fi
}
