#!/usr/bin/env bash
# A disposable LAN host for the latency tests (p4/pytest/test_latency.py): a container on a Docker macvlan network on the
# board's Ethernet segment. It accepts the board's router advertisements, including the route information option for
# the Thread OMR prefix, so it can reach Thread devices through the board without touching the host's own networking.
#
#   tools/lan_ping.sh up     build the image (iputils ping), create the network if missing, start the container
#   tools/lan_ping.sh down   remove the container (and the network, if this script created it)
#   tools/lan_ping.sh status show the container's IPv6 routes (the OMR prefix must appear "via fe80::... proto ra")
#
# Then: PING_PREFIX="docker exec otbr-lan-ping" make test-target TEST=p4/pytest/test_latency.py ...
#
# LAN_IF (default mgmt0) is the host interface on the board's segment (untagged in single mode). LAN_NET reuses an
# existing macvlan network on it (default otbr-p0, the phase 0 one) or creates one with LAN_SUBNET (an IPv6-only range
# unrelated to the board's prefixes). The network has no IPv4, so the pcap_stream load (it pings the board's IPv4
# address) still needs the host.
set -euo pipefail

LAN_IF=${LAN_IF:-mgmt0}
LAN_NET=${LAN_NET:-otbr-p0}
LAN_SUBNET=${LAN_SUBNET:-fdaa:1::/64}
NAME=otbr-lan-ping
IMAGE=otbr-lan-ping:local
MARK=/tmp/otbr-lan-ping.created-network

up() {
    # Built over the default bridge: the macvlan network is IPv6-only on the board's segment and may have no internet.
    printf 'FROM python:3.14-alpine\nRUN apk add --no-cache iputils iproute2\n' | docker build -q -t "$IMAGE" - >/dev/null
    if ! docker network inspect "$LAN_NET" >/dev/null 2>&1; then
        docker network create -d macvlan -o parent="$LAN_IF" --ipv6 --subnet "$LAN_SUBNET" "$LAN_NET" >/dev/null
        echo "$LAN_NET" >"$MARK"
    fi
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    # accept_ra 2: take RAs although Docker leaves forwarding on; rt_info_max_plen 64: install /64 route information.
    docker run -d --rm --name "$NAME" --cap-add NET_ADMIN --cap-add NET_RAW \
        --network "name=$LAN_NET,\"driver-opt=com.docker.network.endpoint.sysctls=net.ipv6.conf.IFNAME.accept_ra=2,net.ipv6.conf.IFNAME.accept_ra_rt_info_max_plen=64\"" \
        "$IMAGE" sleep infinity >/dev/null
    echo "started $NAME on $LAN_NET ($LAN_IF); routes appear with the board's next RA (status)"
}

down() {
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    if [[ -f "$MARK" ]]; then
        docker network rm "$(cat "$MARK")" >/dev/null 2>&1 || true
        rm -f "$MARK"
    fi
}

case "${1:-}" in
up) up ;;
down) down ;;
status) docker exec "$NAME" ip -6 route ;;
*)
    echo "usage: $0 up|down|status" >&2
    exit 2
    ;;
esac
