#!/bin/sh
# Contention stress runner (docs/15 §12.1): launches PARALLEL copies of one
# gtest binary at once, each repeating its suite REPEAT times, for ROUNDS
# rounds. Liveness bugs in the scheduler (lost wakeups, park/retry races)
# only surface when the executor threads are starved of CPU, which a
# single test process on an idle machine never does.
#
#   run_stress.sh <gtest-binary> [parallel] [repeat] [rounds] [timeout-s] [gtest-filter]
#
# Exit 0 when every copy of every round passed within the timeout; any
# failure or hang prints the tail of the offending log and exits 1.
set -u

# Tells the binaries they run under contention: timing budgets (PERF-1 in
# ge_scheduler_test) are recorded but not enforced.
GE_STRESS=1
export GE_STRESS

BIN=${1:?gtest binary}
PARALLEL=${2:-8}
REPEAT=${3:-10}
ROUNDS=${4:-3}
TIMEOUT=${5:-300}
FILTER=${6:-*}

if [ ! -x "$BIN" ]; then
  echo "run_stress: $BIN is not executable" >&2
  exit 2
fi

LOGDIR=${GE_STRESS_LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/ge_stress.XXXXXX")}
mkdir -p "$LOGDIR"
NAME=$(basename "$BIN")
echo "run_stress: $NAME parallel=$PARALLEL repeat=$REPEAT rounds=$ROUNDS timeout=${TIMEOUT}s logs=$LOGDIR"

# Runs one copy under a watchdog: the copy is killed after TIMEOUT seconds
# and the log marked so the round is reported as a hang, not a pass.
run_one() {
  log=$1
  "$BIN" --gtest_filter="$FILTER" --gtest_repeat="$REPEAT" --gtest_brief=1 > "$log" 2>&1 &
  pid=$!
  (
    n=0
    while [ "$n" -lt "$TIMEOUT" ]; do
      sleep 1
      kill -0 "$pid" 2>/dev/null || exit 0
      n=$((n + 1))
    done
    echo "run_stress: TIMEOUT after ${TIMEOUT}s, killing pid $pid" >> "$log"
    kill -KILL "$pid" 2>/dev/null
  ) &
  wd=$!
  wait "$pid"
  rc=$?
  kill "$wd" 2>/dev/null
  wait "$wd" 2>/dev/null
  if [ "$rc" -ne 0 ]; then echo "run_stress: exit=$rc" >> "$log"; fi
}

failed=0
round=1
while [ "$round" -le "$ROUNDS" ]; do
  i=1
  while [ "$i" -le "$PARALLEL" ]; do
    run_one "$LOGDIR/${NAME}.r${round}.p${i}.log" &
    i=$((i + 1))
  done
  wait
  for log in "$LOGDIR"/"${NAME}".r"${round}".p*.log; do
    if grep -qE "FAILED|run_stress: (TIMEOUT|exit=)|ERROR: (Address|Thread|Leak)Sanitizer" "$log"; then
      failed=$((failed + 1))
      echo "run_stress: FAIL $log"
      tail -40 "$log"
    fi
  done
  echo "run_stress: round $round/$ROUNDS done, failures so far: $failed"
  round=$((round + 1))
done

if [ "$failed" -ne 0 ]; then
  echo "run_stress: $NAME FAILED ($failed copies)"
  exit 1
fi
echo "run_stress: $NAME OK"
exit 0
