# SPDX-License-Identifier: GPL-2.0-or-later
# NixOS: let clients of a NetworkManager hotspot ("shared" connection) reach the PC's DHCP and DNS server.
#
# NetworkManager runs dnsmasq for shared connections, but the NixOS firewall drops DHCP/DNS from the clients. Fedora's firewalld
# has an "nm-shared" zone for this; here a dispatcher script opens UDP 67/53 and TCP 53 on the interface only while a shared
# connection is up on it. A firewall reload drops the rules until the connection is brought up again.
#
# Use: add this file to the imports of your NixOS configuration (it works with any interface name).
{ pkgs, ... }:

{
  networking.networkmanager.dispatcherScripts = [
    {
      type = "basic";
      source = pkgs.writeShellScript "nm-shared-firewall" ''
        iface="$1"; action="$2"
        [ "$action" = up ] || [ "$action" = down ] || exit 0
        method=$(${pkgs.networkmanager}/bin/nmcli -g ipv4.method connection show uuid "$CONNECTION_UUID" 2>/dev/null)
        [ "$method" = shared ] || exit 0
        ipt=${pkgs.iptables}/bin/iptables
        for rule in "udp --dport 67" "udp --dport 53" "tcp --dport 53"; do
          if [ "$action" = up ]; then
            $ipt -C nixos-fw -i "$iface" -p $rule -j ACCEPT 2>/dev/null || $ipt -I nixos-fw 1 -i "$iface" -p $rule -j ACCEPT
          else
            $ipt -D nixos-fw -i "$iface" -p $rule -j ACCEPT 2>/dev/null || true
          fi
        done
      '';
    }
  ];
}
