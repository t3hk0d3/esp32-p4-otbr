# HTTP API and web UI

Everything is on port 80, no authentication. Errors on `/device/*` are JSON `{"error": "<code>", "detail": "<text>"}`.

## Device API

| Method and path | Purpose | Notable errors |
| --- | --- | --- |
| `GET /device/status` | mode, app version and slot, `rcp`, `jumper`, `eth` addresses, `pd`, `sniffer` stats, `fault_reason`, `storage_ok` | |
| `GET /device/sniffer.pcap?channel=&duration=` | stream a pcap capture | 400 `invalid_channel`, `invalid_duration`, `no_channel`; 409 `busy` (not `BR_RUNNING`); 503 `busy` (no channel given and OpenThread's state could not be read within 100 ms: retry) |
| `DELETE /device/sniffer` | stop the running capture | 404 `not_found` |
| `POST /device/ota` | upload an `otbr.bundle`, then reboot | 400 `length_required`, `invalid_bundle`; 413 `payload_too_large`; 409 `busy` |
| `POST /device/ota/url` | body `{"url": "http://..."}`, same via download | 400 `invalid_request`, `https_not_supported`; 502 `transfer_failed` |
| `POST /device/radio/firmware[?from_url=]` | push `c6_rcp.bin` to the C6, restart the BR; on a factory C6 (`fault_reason` `esp-hosted firmware`) only `c6_installer.bin` of the same build, sent through ESP-Hosted, then `{"installed":null}` and `no rcp firmware` | 400 `length_required`, `invalid_image` (also `c6_installer.bin` to our updater, anything else to ESP-Hosted, an installer of another build), `https_not_supported`; 413 `payload_too_large`; 409 `busy` (sniffing, starting, update running); 502 `fetch_failed`, `push_failed`, `install_failed` (ESP-Hosted transfer of `c6_installer.bin`) |
| `POST /device/radio/recover` | maintenance jumper: rewrite the C6 base image (no RCP firmware) and restart | 200 `{"recovered":true,"rcp":"<installed version>"\|"no rcp firmware"\|"unknown","mode":...}` (also when the C6 has no RCP firmware afterwards); 412 `no_jumper`; 409 `busy`; 502 `recovery_failed` (only when the recovery itself failed) |
| `GET /device/radio/log` | jumper: the newest 16 KB of the C6 console (`text/plain`) | 412 `no_jumper` |
| `GET /device/diag` | per-core load, per-task CPU/stack/affinity, memory, OpenThread lock stats, uptime, reset reason | |
| `GET /device/network` | active config, pending trial with remaining time, interface state | |
| `PUT /device/network` | store a trial network config and reboot into it | 400 `invalid_config` (`detail` names the field); 409 `busy`; 413 |
| `POST /device/network/confirm` | commit the trial config | 409 `no_trial`; 403 `wrong_interface` |
| `POST /device/reboot` | reboot | 409 `busy` while an update or push runs |
| `GET /metrics` | device health and Thread figures in the Prometheus text format, see [metrics](metrics.md) | 500 `no_memory` |

`/device/status` modes: `STARTING`, `BR_RUNNING`, `SNIFFER`, `RADIO_FAULT`.

## Thread REST API

Upstream esp-thread-br, ot-br-posix 0.6.0 compatible: `/node`, `/node/state`, `/node/dataset/active|pending`,
`/node/ba-id`, `/node/ext-address`, `/node/coprocessor/version`, `/node/ba-epskc/*`, `/node/commissioner/*`, `/api/actions`,
`/api/devices`, `/api/diagnostics`, `/api/node`, `/.well-known/thread/br-rest`. In `SNIFFER` these answer 503 (reads) or 409
(changes); in `STARTING` and `RADIO_FAULT` they answer 503. `DELETE /node/dataset/...` is not supported. `PUT /node/dataset/active`
is accepted while Thread runs (ot-br-posix answers 409): the board stops Thread, stores the dataset and starts Thread on
it (the old network's pending dataset is deleted); the dataset it already has is accepted without a restart. A partial dataset (e.g. only `channel`) still answers 409
while Thread runs: use `/node/dataset/pending` to change a running network. The mode, fault reason
and version information are in `/device/status`, which works in every mode. Home Assistant's OpenThread Border Router integration
connects to `http://<board>` (verified with `python-otbr-api` by `tools/otbr_compat.py`; a run against a real Home Assistant is
still open, see `docs/phase0/RESULTS.md`).

## Web UI

`http://<board>/` serves the upstream pages (Dashboard, Network to form or join, Commissioner, Addresses, Tools, Topology,
About). The Dashboard (`/`) is the status page: mode, versions, radio, Ethernet, prefix delegation, load, and while the
border router runs the Thread network, identifiers and Thread addresses. The "Device" menu holds `/ethernet.html` (single or split setup,
VLANs and addresses, with a review step before the reboot), `/firmware.html` (P4 bundle and C6 radio firmware, radio
recovery with the jumper), `/capture.html` (packet capture) and `/diagnostics.html`. Every device page shows the trial
banner with **Keep new settings** while network settings are on trial. They use only `/device/*`, so they also work in
`RADIO_FAULT` and while sniffing.

## Creating a Thread network

The board never forms or joins a network by itself. Use the Web UI (Network page: form, join a scanned network, or join
with dataset TLVs), Home Assistant (add the
OpenThread Border Router integration with the URL `http://<board>`), or the REST API / `python-otbr-api`:

```
import asyncio, aiohttp, python_otbr_api as api

async def main():
    async with aiohttp.ClientSession() as session:
        otbr = api.OTBR("http://<board>", session)
        await otbr.create_active_dataset(api.models.ActiveDataSet(network_name="MyThread", channel=15))
        await otbr.set_enabled(True)

asyncio.run(main())
```

The dataset is stored on the board and used again at every boot. `GET /node/state` returns `leader` (alone) or
`router`/`child`/`detached`.

### Adding more border routers

Several boards can serve the same Thread network: each one adds coverage, and the others keep the network reachable when
one goes down. Form the network on the first board, then give every other board the same active dataset, either with the
Web UI (Network page, "Join with Dataset": paste the TLVs of the first board) or by copying it over the REST API:

```
curl -H 'Accept: text/plain' http://<first-board>/node/dataset/active > dataset.tlv
curl -X PUT -H 'Content-Type: text/plain' --data-binary @dataset.tlv http://<new-board>/node/dataset/active
curl -X PUT -H 'Content-Type: application/json' -d '"enable"' http://<new-board>/node/state
```

The last call is only needed when Thread was off on the new board (no dataset before): `GET /node/state` says `disabled`.
On a board that is already on a network, the `PUT` of the dataset restarts Thread on the new one by itself, and enabling a
running Thread would start a fresh attach.

In Home Assistant, add each board as its own OpenThread Border Router integration entry. Running several of these boards
on one network has not been tested on hardware yet.
