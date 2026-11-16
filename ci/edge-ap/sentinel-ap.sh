#!/bin/bash
# sentinel-ap.sh - Wi-Fi access point for the Sentinel nodes, and the Edge Server's wired uplink.
# A virtual AP interface (wlan1) on the Wi-Fi chip (wlan0), joined by hostapd to the bridge br0 = 192.168.100.1;
# dnsmasq gives the nodes their addresses. Settings: /etc/sentinel-ap.env. Run at boot by sentinel-ap.service.
set -u
[ -f /etc/sentinel-ap.env ] && . /etc/sentinel-ap.env
PARENT=${AP_PARENT:-wlan0}
AP=${AP_IFACE:-wlan1}
BR=${AP_BRIDGE:-br0}
ADDR=${AP_ADDR:-192.168.100.1/24}
HOSTAPD_CONF=${AP_HOSTAPD_CONF:-/etc/hostapd/sentinel-ap.conf}
UPLINK=${UPLINK_IFACE:-eth0}
UPLINK_ADDR=${UPLINK_ADDR:-}            # e.g. 10.0.0.33/24; empty = leave the uplink as the system configures it
UPLINK_GW=${UPLINK_GW:-}                # e.g. 10.0.0.11 - must forward traffic to the internet (apt, NTP)
UPLINK_DNS=${UPLINK_DNS:-}              # e.g. 1.1.1.1 8.8.8.8
STOP_NM=${AP_STOP_NETWORKMANAGER:-1}    # NetworkManager would take the Wi-Fi chip back

echo "== Wi-Fi access point: $AP on $PARENT, bridge $BR = $ADDR"
[ "$STOP_NM" = 1 ] && systemctl stop NetworkManager 2>/dev/null
pkill wpa_supplicant 2>/dev/null
pkill hostapd 2>/dev/null
sleep 1
ip link set dev "$BR" down 2>/dev/null
ip link delete "$BR" type bridge 2>/dev/null
iw dev "$AP" del 2>/dev/null
ip link set dev "$PARENT" up
iw dev "$PARENT" interface add "$AP" type __ap || { echo "cannot create $AP on $PARENT" >&2; exit 1; }
ip link add name "$BR" type bridge
ip addr add "$ADDR" dev "$BR"
ip link set dev "$BR" up
ip addr flush dev "$AP"                 # the AP interface has no address of its own: hostapd adds it to the bridge
ip link set dev "$AP" up
hostapd -B "$HOSTAPD_CONF" || { echo "hostapd did not start: check $HOSTAPD_CONF" >&2; exit 1; }

if [ -n "$UPLINK_ADDR" ]; then
  echo "== Uplink: $UPLINK = $UPLINK_ADDR, gateway ${UPLINK_GW:-none}, DNS ${UPLINK_DNS:-unchanged}"
  ip addr flush dev "$UPLINK"
  ip addr add "$UPLINK_ADDR" dev "$UPLINK"
  ip link set dev "$UPLINK" up
  [ -n "$UPLINK_GW" ] && ip route replace default via "$UPLINK_GW" dev "$UPLINK"
  if [ -n "$UPLINK_DNS" ]; then
    if command -v resolvectl >/dev/null; then
      # shellcheck disable=SC2086
      resolvectl dns "$UPLINK" $UPLINK_DNS
    else
      printf 'nameserver %s\n' $UPLINK_DNS > /etc/resolv.conf
    fi
  fi
fi

echo "== DHCP for the nodes (dnsmasq)"
systemctl restart dnsmasq || { echo "dnsmasq did not start: journalctl -u dnsmasq" >&2; exit 1; }
echo "== done"
