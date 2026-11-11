#!/usr/bin/env bash
# Run all bounded models. Mutation failures must name the expected invariant.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .tools
jar=.tools/tla2tools.jar
if [[ ! -f $jar ]]; then
  curl -fLsS https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar \
    -o "$jar.part"
  mv "$jar.part" "$jar"
fi
if [[ -n ${JAVA:-} ]]; then
  java_cmd=("$JAVA")
elif java -version >/dev/null 2>&1; then
  java_cmd=(java)
else
  java_cmd=(mise exec java@temurin-25 -- java)
fi
configs=("$@")
if (( ${#configs[@]} == 0 )); then configs=(*.cfg); fi
result=0
for cfg in "${configs[@]}"; do
  cfg=${cfg##*/}
  expected=
  case $cfg in
    MutA_*) expected=LogViewInvariant ;;
    MutB_*) expected=DurableCommit ;;
    MutC_*) expected=Agreement ;;
  esac
  echo "=== $cfg"
  logfile=".tools/${cfg%.cfg}.log"
  set +e
  "${java_cmd[@]}" -XX:+UseParallelGC -Xmx6g -cp "$jar" tlc2.TLC \
    -workers "${TLC_WORKERS:-auto}" -noGenerateSpecTE \
    -metadir ".tools/states/${cfg%.cfg}" -config "$cfg" Vsr.tla \
    >"$logfile" 2>&1
  rc=$?
  set -e
  grep -E '^(Error: Invariant|Model checking completed|[0-9]+ states generated|The depth|Finished in)' "$logfile" || true
  if [[ -z $expected ]] && (( rc == 0 )) && grep -q '0 states left on queue' "$logfile"; then
    echo "PASS: exhaustive within the configured bounds"
  elif [[ -n $expected ]] && (( rc == 12 )) && grep -q "Error: Invariant $expected is violated" "$logfile"; then
    echo "PASS: mutation detected; full trace in $logfile"
  else
    echo "FAIL: unexpected TLC result (exit $rc); see $logfile" >&2
    result=1
  fi
done
exit "$result"
