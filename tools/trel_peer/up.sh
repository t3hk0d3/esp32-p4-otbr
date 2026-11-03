#!/usr/bin/env bash
# Start the TREL test peer: ot-br-posix (official image, TREL built in, started with trel://eth0) using the C6 devkit
# flashed with ot_rcp (tools/trel_peer/flash_c6.sh rcp) as its radio, on the board's backbone link through macvlan.
#
#   LAN_IF       host interface on the board's backbone link (wired: Wi-Fi rejects macvlan). In split mode, the VLAN
#                interface of thread_bb (e.g. enp3s0.20)
#   LAN_SUBNET   IPv4 subnet of that link, e.g. 192.168.1.0/24
#   LAN_GATEWAY  its gateway, e.g. 192.168.1.1
#   PEER_IP      a free IPv4 address on it for the container (outside the DHCP pool)
#   LAN_SUBNET6  optional: the link's IPv6 /64. Without it the container has link-local and SLAAC addresses only
#   RCP_DEVICE   default /dev/ttyACM1 (prefer the /dev/serial/by-id/ path)
#   TREL_PEER    container name, default otbr-trel
#
# The host cannot reach the container's addresses (macvlan); drive it with `docker exec otbr-trel ot-ctl ...`.
set -euo pipefail

: "${LAN_IF:?set LAN_IF}" "${LAN_SUBNET:?set LAN_SUBNET}" "${LAN_GATEWAY:?set LAN_GATEWAY}" "${PEER_IP:?set PEER_IP}"
name="${TREL_PEER:-otbr-trel}"
net="${name}-lan"
image="${OTBR_IMAGE:-openthread/border-router:latest}"
rcp="$(readlink -f "${RCP_DEVICE:-/dev/ttyACM1}")"

# Recreated on each run, so a changed LAN_IF or subnet takes effect.
"$(dirname "$0")/down.sh" > /dev/null
args=(-d macvlan -o parent="${LAN_IF}" --subnet "${LAN_SUBNET}" --gateway "${LAN_GATEWAY}")
if [ -n "${LAN_SUBNET6:-}" ]; then
    args+=(--ipv6 --subnet "${LAN_SUBNET6}")
fi
docker network create "${args[@]}" "${net}"
# On an IPv4-only network Docker turns IPv6 off on the interface: TREL needs it, and RAs for SLAAC.
sysctls="net.ipv6.conf.IFNAME.disable_ipv6=0,net.ipv6.conf.IFNAME.accept_ra=2,net.ipv6.conf.IFNAME.accept_ra_rt_info_max_plen=64"

docker pull "${image}"
docker run -d --name "${name}" \
    --network "name=${net},ip=${PEER_IP},\"driver-opt=com.docker.network.endpoint.sysctls=${sysctls}\"" \
    --device "${rcp}:/dev/rcp" --device /dev/net/tun \
    --cap-add NET_ADMIN --cap-add NET_RAW \
    --sysctl net.ipv6.conf.all.forwarding=1 --sysctl net.ipv4.ip_forward=1 \
    -e OT_RCP_DEVICE="spinel+hdlc+uart:///dev/rcp?uart-baudrate=460800" \
    -e OT_INFRA_IF=eth0 -e OT_THREAD_IF=wpan0 -e OT_LOG_LEVEL=6 \
    "${image}"

for _ in $(seq 60); do
    if docker exec "${name}" ot-ctl state > /dev/null 2>&1; then
        echo "${name} up: $(docker exec "${name}" ot-ctl rcp version | head -1)"
        exit 0
    fi
    sleep 2
done
echo "${name}: otbr-agent did not answer within 120 s; docker logs ${name}" >&2
exit 1
