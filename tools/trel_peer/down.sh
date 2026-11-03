#!/usr/bin/env bash
# Stop the TREL test peer and remove its macvlan network. A removal that fails stops the script with Docker's error.
set -euo pipefail

name="${TREL_PEER:-otbr-trel}"
containers="$(docker ps -a --format '{{.Names}}')"
networks="$(docker network ls --format '{{.Name}}')"
if grep -Fxq "${name}" <<< "${containers}"; then
    docker rm -f "${name}" > /dev/null
fi
if grep -Fxq "${name}-lan" <<< "${networks}"; then
    docker network rm "${name}-lan" > /dev/null
fi
echo "${name} removed"
