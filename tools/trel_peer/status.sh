#!/usr/bin/env bash
# TREL state of the test peer: role, TREL on/off, peers, counters and the radios used per neighbour.
set -euo pipefail

name="${TREL_PEER:-otbr-trel}"
for command in state trel "trel peers" "trel counters" "multiradio neighbor list"; do
    echo "== ${command}"
    # shellcheck disable=SC2086 # the command is split into ot-ctl arguments on purpose
    docker exec "${name}" ot-ctl ${command}
done
