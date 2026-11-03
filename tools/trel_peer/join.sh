#!/usr/bin/env bash
# Join the TREL test peer to the board's Thread network: copy the board's active dataset and start Thread.
# Usage: tools/trel_peer/join.sh <board address>
set -euo pipefail

board="${1:?usage: $0 <board address>}"
name="${TREL_PEER:-otbr-trel}"
ot() { docker exec "${name}" ot-ctl "$@"; }

tlvs="$(curl -fsS -H 'Accept: text/plain' "http://${board}/node/dataset/active")"
ot thread stop > /dev/null
ot ifconfig down > /dev/null
ot dataset set active "${tlvs}" > /dev/null
ot ifconfig up > /dev/null
ot thread start > /dev/null
echo "${name} joining $(ot dataset active | sed -n 's/^Network Name: //p')"
