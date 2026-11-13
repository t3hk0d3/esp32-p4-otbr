# Updates from GitHub on a factory C6: installer, then radio firmware (page-driven)

## Context

On 2026-11-13 a new rev3 board on v0.6.0 with a factory C6 got `POST /device/update` → HTTP 400 `invalid_image`
"C6 runs factory ESP-Hosted: upload c6_installer.bin first". Uploading `c6_installer.bin` by hand worked (the updater
answered after ~4 min), then `c6_rcp.bin` installed in ~2 s. The Updates card should do both steps itself.

Decision (user, 2026-11-13): the page orchestrates the steps in JavaScript with the endpoints that exist; the firmware's
update path stays as it is. Building blocks already on the board:
- `POST /device/radio/firmware?from_url=<url>`: on a factory C6 it accepts only `c6_installer.bin` with this firmware's
  base version (else 400 `invalid_image`, "wrong c6_installer.bin: X, need Y", nothing sent), sends it through the
  ESP-Hosted OTA, waits up to ~3 min for the updater without resetting the C6, and answers `{"installed":null}`.
  Errors: 502 `fetch_failed` (the URL did not answer 200), 502 `install_failed` (OTA refused or timed out), 400
  `invalid_image`, 409 `busy`.
- `POST /device/update`: once the C6 has its updater, it installs the bundle (when newer) and `c6_rcp.bin`, then reboots.
- `/device/status` `fault_reason` `esp-hosted firmware` tells the page the C6 is factory.

## Styleguide

`STYLEGUIDE.md`: astyle for C (`make lint`), `device_api_` prefix, host tests in `<component>/test_host/` named
`"<scenario>: <effect>"`. Page logic lives in the `L` object of `firmware.js`, tested with node through
`tools/tests/test_firmware_page.py` (`FirmwareLogic`, `FirmwarePage`). Docs: guide vs reference split, `docs/api.md` for
fields and error codes.

## Approach

### P4 (one small field, no new flow)

- `GET /device/update`: `release` gains `installer_url`, the release's `c6_installer.bin` URL built like the bundle's
  (`self_update_asset_url(self_update_base(), tag, "c6_installer.bin")`, so a test build's base override is honoured).
  `null` without a release. Built in `device_api_update_json` (`update_json.c`, pure). The page never guesses GitHub
  URLs.
- `rcp_sync`, `run_update` and `POST /device/radio/firmware` are unchanged.

### Page: `firmware.js`

- `installUpdate`, when the last status has `fault_reason` `esp-hosted firmware` and the update has a radio step
  (`rcp_differs`):
  1. `POST /device/radio/firmware?from_url=<release.installer_url>` (no client timeout).
  2. On 200: refresh the status, then `POST /device/update` as today (it now sees the updater: `c6_rcp.bin`, plus the
     bundle and the reboot when the app is newer). Its reply text is prefixed by "Installer done." on success.
  3. On `invalid_image` with a "wrong c6_installer.bin" detail (release base ≠ running base) and the app newer: go on
     with `POST /device/update` anyway; the bundle installs, the radio step fails with the factory refusal (`rcp:
     "failed"`), the board reboots, and the after-reboot line says to press Install again (the new firmware has the
     matching base). With the app not newer: stop with the mismatch text.
  4. On `fetch_failed`: stop; "This release has no c6_installer.bin" (older releases).
  5. On `install_failed`, a lost connection, or any other failure: stop; "The C6 may still be installing: wait about 5
     minutes without powering off the board, then reload. If the radio fault still says esp-hosted firmware, press
     Install again."
- New `L` helpers: `factoryInstall(u, faultReason)` (whether the installer step runs), `installerFailureText(r)`
  (texts of 3–5), `updateQuestion(u, running, faultReason)` and `updateHint(u, faultReason)` gain the factory variants.
- Texts on a factory C6: hint and confirm name `c6_installer.bin` then `c6_rcp.bin`, about 5 minutes, and the power-loss
  warning: don't power off the board meanwhile; a power cut while the installer writes leaves a C6 that only the
  maintenance jumper or a USB-UART adapter can repair. The progress line says the same during step 1.
- `faultHint('esp-hosted firmware')` adds that **Install** on the Updates card does both steps.
- `firmware.html`: no structural change expected (texts are set by script).

### Docs

- `docs/first-install.md` way A: the Updates card as the first way (both steps), the manual uploads as the alternative
  (needed without internet); "about a minute" → "about 4 minutes"; the "Tested with a stand-in" note says the manual
  way ran on an untouched factory C6 and the Updates-card way not yet; troubleshooting bullet.
- `docs/updates.md`: one paragraph for a factory C6.
- `docs/troubleshooting.md`: the `esp-hosted firmware` row names the Updates card.
- `docs/api.md`: `GET /device/update` `release.installer_url`.
- `CHANGELOG.md` entry.

## BDD skeleton

```
device_api_update_json (update_json.c)
    With a release
        - release.installer_url is "<base>/download/<tag>/c6_installer.bin"
        On an empty tag
            - release.installer_url is "<base>/latest/download/c6_installer.bin"
    Without a release
        - release is null (no installer_url)
    On a buffer too small for installer_url
        - returns -1

FirmwareLogic (firmware.js)
    factoryInstall
        On fault_reason 'esp-hosted firmware', rcp_differs, an installer_url
            - true
        On another fault_reason, no rcp_differs, or no installer_url
            - false
    updateHint
        On a factory install
            - names c6_installer.bin then c6_rcp.bin and about 5 minutes
        Otherwise
            - text as today
    updateQuestion
        On a factory install, radio only
            - names both files, about 5 minutes, the power-loss warning (jumper or USB-UART repair)
        On a factory install with the app newer
            - the reboot question plus the power-loss warning
        Otherwise
            - text as today ("Install the radio firmware of ... from GitHub?")
    installerFailureText
        On 502 fetch_failed
            - "This release has no c6_installer.bin"
        On 400 invalid_image "wrong c6_installer.bin: X, need Y"
            - names both base versions
        On 502 install_failed, or no reply
            - may still be installing: wait about 5 minutes, don't power off, reload; Install again if still esp-hosted
    faultHint
        On 'esp-hosted firmware'
            - mentions Install on the Updates card

FirmwarePage (installUpdate)
    On a factory C6, installer 200
        - POSTs /device/radio/firmware?from_url=<installer_url> first, then POST /device/update
        - the progress line during the installer names about 5 minutes and "don't power off"
        - the final line starts with the installer being done
    On a factory C6, installer fetch_failed
        - no POST /device/update; red line "This release has no c6_installer.bin"
    On a factory C6, installer install_failed or a lost connection
        - no POST /device/update; red line "may still be installing ... wait ... reload"
    On a factory C6, wrong c6_installer.bin, app newer
        - POST /device/update follows; after the reboot the radio failure line says Install again
    On a factory C6, wrong c6_installer.bin, app same
        - no POST /device/update; red line names both base versions
    On the confirm declined
        - nothing is POSTed
    On a C6 with its updater
        - only POST /device/update, as today
```

## Verification

- `make test-host`, `make test-tools`, `make lint`; outputs teed to the scratchpad.
- `make build` for the default target (compile check of the `update_json.c` change).
- On-board (separate pass the user approves, not in this task): a factory C6 on `p4-otbr-rev3/p4-otbr-b` (stand-in via
  `c6_stock_restore` over the jumper, or a factory board), then Device > Firmware > Install from GitHub with the same
  app and with an older one; check both steps, the texts, the lost-connection path.

## Out of scope

- The browser flasher's "propose radio firmware install" (separate session); it can reuse `release.installer_url` or
  its own release URLs.
- Changes to `rcp_sync` or `POST /device/update`.
