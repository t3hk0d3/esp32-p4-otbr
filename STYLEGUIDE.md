# Styleguide

Applies to all code in this repository. Enforced by `uvx pre-commit run --all-files`
(see `.pre-commit-config.yaml`).

## Formatting

- **C:** ESP-IDF's astyle rules (`tools/astyle-rules.yml`, copied from ESP-IDF v6.1
  `tools/ci/astyle-rules.yml`), run through `pre-commit` (`astyle_py`).
- **Python (tools, pytest):** `ruff` lint and `ruff format`.
- **Vendored code** (`p4/components/esp_ot_br_server/`, `p4/components/esp_br_http_ota/`) keeps
  upstream formatting and is excluded from formatting. Every change to it is listed in
  `p4/components/PATCHES.md`.

## Naming

- `snake_case` everywhere.
- Public symbols are prefixed with their component name: `br_core_`, `rcp_sync_`, `sniffer_`,
  `device_api_`, `diag_`, `netcfg_`, `rcp_sync_proto_`.
- One `static const char *TAG` per file, named after the component.

## Errors

- Functions return `esp_err_t`.
- Propagate with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`.
- `ESP_ERROR_CHECK` only for unrecoverable init in `app_main`.

## Config

- Fixed hardware facts (pins, clock rates, flash layout) are constants in headers, not Kconfig.

## Tests

- Host tests live in `<component>/test_host/`, use Unity on the ESP-IDF `linux` target, and are
  named `TEST_CASE("<scenario>: <effect>", "[<component>]")`.
- Target tests live in `p4/pytest/test_<feature>.py` and use `pytest-embedded`.
- Test seams (ops structs) are allowed only where a host test needs them.

## Build mechanisms

- Every project's top-level `CMakeLists.txt` includes, in this order: ESP-IDF's `project.cmake`,
  `tools/version.cmake`, `tools/test_build.cmake`, then calls `project()`.
- `tools/version.cmake` sets `PROJECT_VER`. The stamp is taken at CMake configure time, not at
  every build; the git HEAD files are configure dependencies, so a new commit reconfigures by itself. `-dirty` is as of
  the last configure (`idf.py reconfigure` refreshes it).
- `c6_updater` includes `tools/base_version.cmake` instead of `tools/version.cmake`: its version is content-addressed
  (`base-<8 hex>`, a hash of the sources that define the base image), so it does not change with the build time or git.
- `tools/test_build.cmake` turns the CMake variable `OTBR_TEST_BUILD` into the C macro
  `OTBR_TEST_BUILD=1` for all components. `tools/build_all.sh` passes `-DOTBR_TEST_BUILD=0|1` on
  every run, so the cached value never sticks. Test-only code is guarded by `#ifdef OTBR_TEST_BUILD`.
