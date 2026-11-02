#!/bin/sh
# Turn a failed step's log into GitHub annotations, which show on the run's
# summary page (and in the API) without opening the raw log: one per compiler
# error, plus the log's tail as a final one.
LOG="$1"
[ -f "$LOG" ] || exit 0
grep -E '^(\.\./)?src/[^:]+:[0-9]+:[0-9]+: (fatal )?error: ' "$LOG" | head -8 |
    sed -E 's#^(\.\./)?(src/[^:]+):([0-9]+):([0-9]+): (fatal )?error: (.*)#::error file=\2,line=\3,col=\4::\6#'
tail -n 40 "$LOG" | sed 's/%/%25/g' | awk 'BEGIN { printf "::error title=log tail::" }
    { printf "%s%%0A", $0 } END { print "" }'
