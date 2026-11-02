# Documentation

Setting up:

- [First install](first-install.md): bring a new board up (no wires, maintenance jumper or USB-UART) and create a Thread
  network.
- [Building and flashing](building.md): prerequisites, `make` targets, build outputs, versions, USB flashing.

Running it:

- [Updates](updates.md): P4 firmware and web UI over the network, C6 radio firmware, compatibility.
- [HTTP API and web UI](api.md): the `/device/*` API, the Thread REST API, the web pages, creating a network.
- [Network modes, VLANs and prefix delegation](networking.md): single and split mode, confirm-or-revert, DHCPv6-PD.
- [Sniffer](sniffer.md): 802.15.4 captures into Wireshark.
- [Maintenance jumper](maintenance-jumper.md): C6 recovery and console capture.
- [Diagnostics](diagnostics.md): CPU, memory, tasks and OpenThread lock statistics.
- [Prometheus metrics](metrics.md): `GET /metrics`, scrape config, the metric list and example alerts.
- [Troubleshooting and limitations](troubleshooting.md): fault reasons, common problems, known limitations.

Working on it:

- [Development](development.md): repository layout, vendored code, tests, CI and releases.
- `STYLEGUIDE.md`: conventions. `docs/superpowers/`: design spec and implementation plan. `docs/phase0/RESULTS.md`:
  hardware facts from the bring-up.
