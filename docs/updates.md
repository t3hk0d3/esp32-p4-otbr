# Updates

The P4 firmware (app and web UI) and the C6 radio firmware are updated separately, both over the network.

## P4 and web UI (OTA)

```
make ota BOARD=<board>      # builds dist/otbr.bundle from the current build and uploads it
```

or upload `otbr.bundle` from a release on the Firmware page (`http://<board>/firmware.html`, "Border router"), or
`curl --data-binary @otbr.bundle http://<board>/device/ota`. The bundle holds the web image and the app image; both are
written to the inactive pair, the web image is read back and checked against its SHA-256, and only then the boot partition
is switched. The reply is 200 and the board reboots about a second later. A new app that does not run for 60 s with the
HTTP server listening is rolled back by the bootloader. `tools/make_bundle.py --help` shows how to inspect a bundle.

<img src="images/firmware.png" width="640" alt="Firmware page: the Border router (ESP32-P4) and Radio (ESP32-C6) cards with the installed versions and an install box each, and the Radio recovery card with the jumper fitted">

To let the board fetch the bundle itself: `curl -X POST -d '{"url": "http://host/otbr.bundle"}' http://<board>/device/ota/url`.
Both `http://` and `https://` URLs work. An `https://` server is verified against the certificate bundle built into the firmware (the common root set, which covers GitHub and its download host); certificate expiry dates are not checked, because the board has no clock source. Any other scheme gives 400 `invalid_request`.

Updates are accepted in every mode (also in `RADIO_FAULT` and while sniffing). One update or radio push at a time.

## C6 radio firmware

The RCP firmware is not part of the P4 image and never stored on the P4. Update it independently:

```
make radio BOARD=<board>    # uploads c6_rcp/build/c6_rcp.bin
```

or "Radio" on the Firmware page, or with a `c6_rcp.bin` from a release:

```
curl --data-binary @c6_rcp.bin http://<board>/device/radio/firmware
curl -X POST "http://<board>/device/radio/firmware?from_url=http://host/c6_rcp.bin"
```

The reply is `{"installed": "<version>"}`.

What happens: the border router stops (Thread and OpenThread are shut down), the P4 resets the C6 into its updater,
streams the image to it frame by frame (nothing is buffered on the P4), the C6 checks it and switches its boot slot
only when the transfer is complete, then the C6 resets and OpenThread starts again. Expect a few seconds of outage;
the Thread network keeps running on the other routers meanwhile, and an end device whose parent is the border router
loses its parent for that time. The Thread dataset stays stored and is used again. A failed upload (cable pulled, C6 reset
mid-transfer) leaves the previous firmware in place and the border router restarts with it; upload again. With `from_url`
the P4 retries the whole transfer up to three times.

## Versions and compatibility

`GET /device/status` (`make status BOARD=<board>`) shows `rcp.installed` (version string reported by the C6 updater: a
non-empty string, `""` when the C6 has no valid RCP firmware, `null` if the probe failed) and `rcp.compatible`. The
version is only informative: nothing is compared or ordered. Compatibility is decided by OpenThread itself when it
starts: it rejects an RCP that does not match the OpenThread build of the P4 (spinel/RCP API version), and the board goes
to `RADIO_FAULT` with the reason `rcp incompatible` (`rcp.compatible` false). The cure is the same as for a missing
firmware: upload a matching `c6_rcp.bin` (from the same release as the P4 bundle, or one the release notes say is
compatible). A C6 without any RCP firmware gives `RADIO_FAULT` with `no rcp firmware`.

A P4 update runs the automatic ROM recovery only when the content-addressed base image version changed (see
[Maintenance jumper](maintenance-jumper.md)); the RCP firmware survives it.
