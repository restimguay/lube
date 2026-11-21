#!/bin/bash
# tests/soak/run.sh [SECONDS] [MODEL] [MODEL2]: runs soak.lube for SECONDS (3600 unless given: an hour) and watches
# it, into build/soak: its output (rounds.txt), its resident memory, the load and the free disk every 5 s (mem.csv),
# and on macOS its physical footprint every minute (foot.csv) and `leaks` against it every 10 minutes
# (leaks_live.txt). MODEL is a GGUF file for llm (one of tests/models' tiny ones unless given), MODEL2 a second.
#   LUBE=./lube-check tests/soak/run.sh 60     the leak-checking build: it fails at exit if any object is alive
#   python3 tests/soak/analyze.py build/soak   what the run says
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${1:-3600}
MODEL=$(cd "$(dirname "${2:-$ROOT/tests/models/llama-mixed.gguf}")" && pwd)/$(basename "${2:-llama-mixed.gguf}")
MODEL2=${3:+$(cd "$(dirname "$3")" && pwd)/$(basename "$3")}
LUBE=${LUBE:-$ROOT/lube}
case $LUBE in /*) ;; *) LUBE=$PWD/$LUBE ;; esac  # (it runs from build/soak)
OUT=$ROOT/build/soak
mkdir -p "$OUT/tmp"
cd "$OUT" || exit 1
rm -f rounds.txt mem.csv foot.csv leaks_live.txt stderr.txt
"$LUBE" "$ROOT/tests/soak/soak.lube" "$MODEL" "$SECS" tmp "$MODEL2" > rounds.txt 2> stderr.txt &
pid=$!
echo "t,rss_kb,load1,disk_free_mb" > mem.csv
echo "t,phys_footprint,phys_footprint_peak" > foot.csv
macos=$(command -v footprint > /dev/null && command -v leaks > /dev/null && echo 1)
start=$(date +%s)
n=0
while kill -0 $pid 2> /dev/null; do
    t=$(($(date +%s) - start))
    rss=$(ps -o rss= -p $pid | tr -d ' ')
    load=$(uptime | sed 's/.*load average[s]*: //' | tr -s ', ' ' ' | cut -d' ' -f1)
    disk=$(df -m . | awk 'NR==2{print $4}')
    [ -n "$rss" ] && echo "$t,$rss,$load,$disk" >> mem.csv
    if [ -n "$macos" ] && [ $((n % 12)) -eq 0 ]; then
        footprint -p $pid --summary 2> /dev/null |
            awk -v t=$t '/phys_footprint:/{f=$2$3} /phys_footprint_peak:/{p=$2$3} END{if (f) print t","f","p}' >> foot.csv
    fi
    if [ -n "$macos" ] && [ $((n % 120)) -eq 60 ]; then
        echo "t=$t $(leaks $pid 2> /dev/null | grep -E 'leaks for')" >> leaks_live.txt
    fi
    n=$((n + 1))
    sleep 5
done
wait $pid
status=$?
echo "exit $status" >> stderr.txt
echo "soak: exit $status after $(($(date +%s) - start)) s (build/soak; python3 tests/soak/analyze.py build/soak)"
exit $status
