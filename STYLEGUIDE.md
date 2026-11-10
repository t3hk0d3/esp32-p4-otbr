# Styleguide

Applies to all code in this repository. Enforced by `make lint` (`uvx pre-commit run --all-files`,
see `.pre-commit-config.yaml`).

## Formatting

- **C:** ESP-IDF's astyle rules (`tools/astyle-rules.yml`, copied from ESP-IDF v6.1
  `tools/ci/astyle-rules.yml`), run through `pre-commit` (`astyle_py`).
- **Python (tools, pytest):** `ruff` lint and `ruff format`.
- **Vendored code** (`p4/components/esp_ot_br_server/`, `p4/components/esp_br_http_ota/`,
  `p4/components/usb_host_cdc_acm/`) keeps
  upstream formatting and is excluded from formatting. Every change to it is listed in
  `p4/components/PATCHES.md`.

## Naming

- `snake_case` everywhere.
- Public symbols are prefixed with their component name: `br_core_`, `rcp_sync_`, `sniffer_`,
  `device_api_`, `self_update_`, `diag_`, `netcfg_`, `netnames_`, `metrics_`, `rcp_sync_proto_`, `radio_mux_`, `rcp_proxy_`,
  `zigbee_`, `builtin_rcp_`, `timesync_`; `c6_hosted` uses `c6_hosted_`.
- One `static const char *TAG` per file, named after the component.

## Errors

- Functions return `esp_err_t`.
- Propagate with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`.
- `ESP_ERROR_CHECK` only for unrecoverable init in `app_main`.

## Tasks and locks

- Code that runs on the OpenThread task (callbacks, CLI output) or the esp_timer task never calls lwIP, directly or
  through `httpd_queue_work`: lwIP's thread waits for the OpenThread lock to send to Thread. Notify a task instead.

## Config

- Fixed hardware facts (pins, clock rates, flash layout) are constants in headers, not Kconfig.

## Tests

- Host tests live in `<component>/test_host/`, use Unity on the ESP-IDF `linux` target, and are
  named `TEST_CASE("<scenario>: <effect>", "[<component>]")`.
- Target tests live in `p4/pytest/test_<feature>.py` and use `pytest-embedded`.
- Test seams (ops structs) are allowed only where a host test needs them.

## Build mechanisms

- Every project's top-level `CMakeLists.txt` includes, in this order: ESP-IDF's `project.cmake`,
  `tools/version.cmake`, `tools/test_build.cmake`, then calls `project()`. `p4` also includes
  `tools/ot_radio_port.cmake` (the generated openthread component) after `tools/test_build.cmake`.
- `tools/version.cmake` sets `PROJECT_VER`. The stamp is taken at CMake configure time, not at
  every build; the git HEAD files are configure dependencies, so a new commit reconfigures by itself. `-dirty` is as of
  the last configure (`make reconfigure` refreshes it).
- `c6_updater` includes `tools/base_version.cmake` instead of `tools/version.cmake`: its version is content-addressed
  (`base-<8 hex>`, a hash of the sources that define the base image), so it does not change with the build time or git.
- `tools/test_build.cmake` turns the CMake variable `OTBR_TEST_BUILD` into the C macro
  `OTBR_TEST_BUILD=1` for all components. `tools/build_all.sh` passes `-DOTBR_TEST_BUILD=0|1` on
  every run, so the cached value never sticks. Test-only code is guarded by `#ifdef OTBR_TEST_BUILD`.

## Commands and docs

- The `Makefile` is the entry point for every routine task (build, flash, updates, packaging, tests, lint); its targets
  call the scripts in `tools/`. Documentation and CI use the make targets, not the scripts or `idf.py` directly. A new
  routine task gets a target with a `## ` help comment.
- `README.md` is a short overview; the documentation lives in `docs/`, one file per topic, indexed in `docs/README.md`.
- Every page in `docs/` is either a **guide** or **reference**:
    - A guide is for someone who runs the board from a release download. It says what the feature is for, then how
      to use it in the web UI, then the `curl` call, and links to the reference for fields and error codes. It has
      no source paths, test names, test-build commands, dated bench results or internals. A sentence on a limit
      the reader can hit stays; the reason behind it goes to the reference.
    - Reference pages (`docs/api.md`, `docs/metrics.md`, `docs/reference/`) list endpoints, fields, states and
      internals. On-board check lists and measurements go to `docs/development.md`.
- Release users never need `make`: a guide gives the release-file way first and the checkout way (`make ...`) as a
  short note after it.
- Words: **radio** is a radio on the board (the C6 or a USB dongle, `builtin-c6` / `usb-...` in the API); **Thread
  channel** or **Zigbee channel** is the 802.15.4 channel 11 to 26. Jargon is defined in `docs/glossary.md` and linked
  at its first use on a page.
- Images live in `docs/images/`. Diagrams come from `tools/docs_diagrams.py docs/images`. Screenshots are PNG, taken from the preview server (`tools/docs_preview.py`) where it
  can show the page, and carry an `alt` text that says what the reader should notice. Diagrams are standalone SVG in
  the style of `docs/images/hero.svg`: their own `<style>` with a light and a dark scheme, teal for radio, copper for
  wire.
