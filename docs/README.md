# Documentation

Get started:

- [Flash in the browser](flash.md): write the board's firmware from the documentation site, no tools needed.
- [First install](first-install.md): flash the board, put the radio firmware on its C6, give it a Thread network.
- [Home Assistant](home-assistant.md): add the board, move it onto the preferred network, phones and Matter.
- [Your Thread network](thread-network.md): form or join a network, more border routers, choosing the leader.

Using the board:

- [Web UI tour](web-ui.md): every page of the board's web UI and what it is for.
- [Radios and USB dongles](radios.md): Thread, Zigbee or RCP proxy on the C6 or a dongle; moving Thread between radios.
- [Zigbee coordinator](zigbee.md) (experimental): the board as a Zigbee coordinator for ZHA.
- [Networking](networking.md): single port or split VLANs, confirm-or-revert, names, Thread over Ethernet (TREL).
- [Routing to the mesh](routing.md): DHCPv6 prefix delegation, a custom OMR prefix, BGP and RIPng.
- [Updates](updates.md): from GitHub in one click, or from files.
- [Packet capture](sniffer.md): 802.15.4 captures into Wireshark.
- [Topology and console](topology.md): the mesh map and the OpenThread command line in the browser.
- [Monitoring](diagnostics.md): the Diagnostics page; [Prometheus metrics](metrics.md) for history and alerts.
- [Logging](logging.md): log levels per component, syslog, the live log in the browser, the `log` console command.
- [Maintenance jumper](maintenance-jumper.md): radio repair and its console.
- [Troubleshooting](troubleshooting.md): what the Dashboard's faults mean, common problems, limitations.
- [Glossary](glossary.md).

Reference:

- [HTTP API](api.md): every endpoint, its fields and errors.
- [Prometheus metrics](metrics.md): the metric list, scrape config and example alerts.
- [Internals](reference/internals.md): how radios, the C6's maintenance, updates, diagnostics and logging work.

Working on it:

- [Building and flashing](building.md): prerequisites, `make` targets, build outputs, versions.
- [Development](development.md): repository layout, vendored code, tests, on-board checks, CI, releases and this site.
- `STYLEGUIDE.md`: conventions. `docs/superpowers/`: design specs and implementation plans. `docs/phase0/RESULTS.md`:
  hardware facts from the bring-up.
