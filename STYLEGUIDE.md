# Styleguide

Applies to all code in this repository. Enforced by `make lint` (`uvx pre-commit run --all-files`,
see `.pre-commit-config.yaml`).

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
  `device_api_`, `self_update_`, `diag_`, `netcfg_`, `netnames_`, `metrics_`, `rcp_sync_proto_`; `c6_hosted` uses `c6_hosted_`.
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
  `tools/version.cmake`, `tools/test_build.cmake`, then calls `project()`.
- `tools/version.cmake` sets `PROJECT_VER`. The stamp is taken at CMake configure time, not at
  every build; the git HEAD files are configure dependencies, so a new commit reconfigures by itself. `-dirty` is as of
  the last configure (`make reconfigure` refreshes it).
- `c6_updater` includes `tools/base_version.cmake` instead of `tools/version.cmake`: its version is content-addressed
  (`base-<8 hex>`, a hash of the sources that define the base image), so it does not change with the build time or git.
- `tools/test_build.cmake` turns the CMake variable `OTBR_TEST_BUILD` into the C macro
  `OTBR_TEST_BUILD=1` for all components. `tools/build_all.sh` passes `-DOTBR_TEST_BUILD=0|1` on
  every run, so the cached value never sticks. Test-only code is guarded by `#ifdef OTBR_TEST_BUILD`.

## Case model (`case/`)

- **Layout:** the model is the Python package `case/p4_case/`; its tests are `case/tests/test_<module>.py`.
  `case/sketches/` holds throwaway sketch scripts and is never imported by the model.
- **Coordinates:** everything is in the *board frame* of the vendor STEP: X 0–55 (button edge at 0), Y 0–68 (cable edge
  at 0), Z 0 at the PCB's top face. Case heights come from `params.case_z(board_z)`, never from a second offset.
- **Constants:** every case dimension is a named upper-case constant in `p4_case/params.py`, with the spec section in a
  comment; board facts are constants in `p4_case/board.py`, checked against the STEP at test time. Geometry code has
  no magic numbers.
- **Functions:** geometry builders are pure (`build_*() -> Part`); only `build.py` and `board.load_board` touch files.
  Checks return `CheckResult(name, ok, detail)` and do not raise for a failed check, only for bad input.
- **Tests:** a pytest class per scenario with the docstring `"<module>.<function>: <scenario>"`, one `test_<effect>`
  method per effect (as in `tools/tests/`).
- **Committed outputs:** the print STLs in `docs/models/` are the only generated files in git. `make case` refreshes
  them, and `case/tests/test_build.py` fails while they differ from a fresh build.
- **Dependencies:** pinned once, in the Makefile's `CASE_PY` (`uv run`); nothing is installed globally.

## Commands and docs

- The `Makefile` is the entry point for every routine task (build, flash, updates, packaging, tests, lint); its targets
  call the scripts in `tools/`. Documentation and CI use the make targets, not the scripts or `idf.py` directly. A new
  routine task gets a target with a `## ` help comment.
- `README.md` is a short overview; the documentation lives in `docs/`, one file per topic, indexed in `docs/README.md`.
