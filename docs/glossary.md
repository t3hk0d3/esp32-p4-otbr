# Glossary

**802.15.4**
:   The low-power radio standard that Thread and Zigbee both use, on channels 11 to 26 in the 2.4 GHz band.

**Border router**
:   A device that connects a Thread network to your home's Ethernet or Wi-Fi network, so Home Assistant and phones can
    reach Thread devices. This board is one. A network can have several.

**C6, P4**
:   The two Espressif chips on the board. The **ESP32-P4** runs the border router, the web UI and everything else; the
    **ESP32-C6** is the board's built-in 802.15.4 radio.

**Channel**
:   An 802.15.4 frequency, 11 to 26. These pages say **Thread channel** or **Zigbee channel**. (The board's API also
    calls each *radio* a "channel"; the guides say **radio** for that.)

**Dataset**
:   Everything that defines a Thread network: its name, Thread channel, network key, PAN ID and a few more IDs. Every
    border router of a network has the same dataset. Copying it to a board puts the board on that network.

**ESP-Hosted**
:   Espressif's factory firmware on the board's C6, which this project replaces once at the first install.

**Leader**
:   The one router in a Thread network that keeps the network's shared data. Any router can take the role.

**Maintenance jumper**
:   Three optional wires that let the P4 rewrite the C6 by itself and read its console. See
    [Maintenance jumper](maintenance-jumper.md).

**mDNS**
:   How devices on a local network find each other by name (`esp-otbr-xxxxxx.local`) and by service, without a DNS
    server. Home Assistant finds border routers this way. It does not cross routers or VLANs.

**NTP**
:   Network Time Protocol: how the board asks a time server for the date. See [Time](networking.md#time).

**OMR prefix**
:   Off-mesh routable prefix: the IPv6 prefix Thread devices take the address from that the rest of your network can
    reach. See [Routing to the mesh](routing.md).

**OTA**
:   Over-the-air update: installing new firmware over the network instead of over USB.

**PD (DHCPv6 prefix delegation)**
:   A way for your router to hand the board an IPv6 prefix for the Thread network, and route it there.

**RCP, radio co-processor**
:   A radio that runs only the low-level part of Thread; the rest runs on a host (here the P4). `c6_rcp.bin` is the C6's
    RCP firmware. Many USB dongles can run OpenThread RCP firmware too.

**RCP proxy**
:   A mode in which the board hands a radio's serial line to another computer over the network. See
    [Radios](radios.md#rcp-proxy).

**Spinel**
:   The protocol between an OpenThread host and its RCP. A dongle that answers spinel runs OpenThread RCP firmware.

**Split mode**
:   A network setup with the management web UI and the Thread backbone on separate VLANs. See
    [Networking](networking.md).

**SRP**
:   Service Registration Protocol: how Thread devices (Matter, HomeKit, ESPHome) register their services with a border
    router, which then announces them over mDNS.

**Syslog**
:   The standard way for devices to send their log lines over the network to a log server (rsyslog, syslog-ng,
    Graylog, ...), one UDP message per line. See [Logging](logging.md#syslog).

**Thread**
:   A low-power IPv6 mesh network for smart home devices, used by Matter.

**TREL**
:   Thread over Ethernet (Thread Radio Encapsulation Link): border routers of one Thread network on the same Ethernet
    link talk to each other over it, joining separate radio areas into one mesh. See
    [Networking](networking.md#trel-thread-over-ethernet).

**ULA**
:   Unique local address: a private IPv6 prefix (`fd00::/8`). The board makes one for the Thread network when no
    other prefix is available.

**Updater**
:   The small program on the C6 that receives new radio firmware from the P4.

**Ziggurat**
:   The Zigbee stack the board runs in Zigbee mode, serving Home Assistant's ZHA. See [Zigbee coordinator](zigbee.md).

**ZHA**
:   Zigbee Home Automation, Home Assistant's built-in Zigbee integration.
