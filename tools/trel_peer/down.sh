#!/usr/bin/env bash
# Stop the TREL test peer and remove its macvlan network.
set -euo pipefail

name="${TREL_PEER:-otbr-trel}"
docker rm -f "${name}" > /dev/null 2>&1 || true
docker network rm "${name}-lan" > /dev/null 2>&1 || true
echo "${name} removed"
