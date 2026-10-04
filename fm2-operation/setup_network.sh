#!/usr/bin/env bash
set -euo pipefail
source config.txt

# Run with sudo. Override HOST_ADDR on another host to avoid duplicate IPs.
NIC_NAME=${NIC_NAME-enp24s0f1np1} # Set to the correct network name for bridge
HOST_ADDR=${HOST_ADDR:-10.0.0.254/24}
BR=br-private

if [[ -n "$NIC_NAME" ]]; then
    ip link show dev "$NIC_NAME" >/dev/null
    if [[ -n "$(ip -o -4 addr show dev "$NIC_NAME" scope global)" ||
          -n "$(ip -4 route show default dev "$NIC_NAME")" ]]; then
        echo "Refusing to bridge $NIC_NAME: it has an IPv4 address or default route." >&2
        exit 1
    fi
fi

if ! ip link show dev "$BR" >/dev/null 2>&1; then
    ip link add "$BR" type bridge
fi
ip addr replace "$HOST_ADDR" dev "$BR"
ip link set "$BR" up
if [[ -n "$NIC_NAME" ]]; then
    ip link set "$NIC_NAME" master "$BR"
    ip link set "$NIC_NAME" up
fi
for tap in tap0 tap1; do
    if ! ip link show dev "$tap" >/dev/null 2>&1; then
        ip tuntap add dev "$tap" mode tap multi_queue
    fi
    ip link set "$tap" master "$BR"
    ip link set "$tap" up
done
ip -br -4 addr show dev "$BR"
