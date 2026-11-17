# P4 rev3 first-boot panic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On P4 v3.x boards, the first boot after a flash write no longer panics, so an OTA update is kept instead of
rolled back. A new known-issues page describes the problem.

**Architecture:** The cause is ESP-IDF v6.1 [issue 19369](https://github.com/espressif/esp-idf/issues/19369).
- While CPU0 starts FreeRTOS, CPU1 waits in an empty loop in `esp_startup_start_app_other_cores` (IDF
  `components/freertos/app_startup.c`).
- On a P4 v3.2 that loop starves CPU0's cache. CPU0's stack `memset` in `prvInitialiseNewTask` then gets a Store
  access fault (MCAUSE 7, MEPC 0x4fc1a572).
- In a spike on the NANO-DB, an `esp_rom_delay_us(10)` in the loop fixed it: first boots went from 4 of 4 panicking to
  0 of 3.

The fix uses the repository's existing way of patching IDF, "Generated ESP-IDF copies" (`tools/ot_radio_port.cmake`),
instead of checking a 4.3 MB copy of `freertos` into `p4/components`:
- A script runs at configure time and writes a `freertos` component into the build directory. The component holds
  symlinks to IDF's files, plus a patched copy of `app_startup.c`.
- The IDF tree itself is not touched.
- If an IDF update changes the patched loop, the configure step fails and names the file. Whoever moves IDF then has
  to look at the patch again.

**Tech Stack:** ESP-IDF v6.1, CMake, pytest (host test of the CMake script), MkDocs.

**Spec:** this plan, ESP-IDF issue 19369 (with our data in issuecomment-6931283132), and the root-cause notes from
2026-11-16 in the more-boards SDD ledger. There is no separate design doc.

**Styleguide:** `STYLEGUIDE.md` (`make lint`).
- The CMake script follows `tools/ot_radio_port.cmake`: a comment header, `<prefix>_link_all`, `<prefix>_patch` and
  `<prefix>_write` helpers, and a `FATAL_ERROR` that names the IDF file when a text to replace is gone.
- Python tests are checked with `ruff`.
- A host test of a CMake script uses a throwaway CMake project, as `tools/tests/test_target_cmake.py` does.
- Docs have one page per topic, indexed in `docs/README.md` and `mkdocs.yml`.
- Every IDF patch gets an entry in `p4/components/PATCHES.md`.

## Global Constraints

- **The patch:** the patched text is IDF v6.1's loop `while (port_xSchedulerRunning[0] == 0) {` with its empty body
  `;`. The new body is `esp_rom_delay_us(10);`. `#include "esp_rom_sys.h"` goes after the file's last existing
  `#include`.
- **Scope:** the patch applies to every P4 target, rev1.3 included. That keeps one behaviour for all images, and a
  10 µs pause in a startup wait costs nothing. Only the `p4` project gets it: the C6 projects are single-core
  (`CONFIG_FREERTOS_UNICORE`), so this code is not compiled there.
- **Location:** the generated component lives at `${CMAKE_BINARY_DIR}/idf_patch_components/freertos`. It is appended
  to `EXTRA_COMPONENT_DIRS` and overrides IDF's `freertos` by name.
- **Include order:** `p4/CMakeLists.txt` includes `tools/freertos_startup_patch.cmake` right after
  `tools/ot_radio_port.cmake`.
- **Missing text:** every replaced text is looked up first. If one is missing, the configure step stops with
  `FATAL_ERROR` "freertos_startup_patch: a text to replace is not in <IDF file> any more".
- **Known-issues page:** `docs/known-issues.md`, a guide. The repository has no `KNOWN_ISSUES` file, and the styleguide
  puts documentation in `docs/`, one file per topic.
- **No flashing inside tasks.** The on-board pass (Task 3) comes after review and fixes.

## Review Focus

1. **An IDF bump changes `app_startup.c`.** Expected: the configure step fails and names the file; the build does not
   quietly go back to IDF's original loop. Test: the missing-text scenario.
2. **A reconfigure with nothing changed.** Expected: `app_startup.c` is not rewritten (same mtime), so `freertos` is
   not recompiled. Test: the reconfigure scenario.
3. **IDF removes or adds a file in `components/freertos`.** Expected: a dangling symlink is removed and a new file is
   linked. Test: the link scenarios.
4. **The override takes effect in a real build.** Expected: the build log names the generated component, and
   `app_startup.c` is compiled from the build directory. Task 1 step 6 checks this.
5. **A rev1.3 board runs the patched image.** Expected: it boots as before. Task 3 checks this.

## BDD skeleton

```
tools/freertos_startup_patch.cmake
    configure (included from p4/CMakeLists.txt)
        IDF freertos has the v6.1 wait loop
            - writes <build>/idf_patch_components/freertos/app_startup.c with esp_rom_delay_us(10) as the loop body
            - the written app_startup.c includes "esp_rom_sys.h"
            - the written app_startup.c is otherwise identical to IDF's
            - every other entry of IDF's components/freertos is a symlink in the generated directory
            - appends <build>/idf_patch_components to EXTRA_COMPONENT_DIRS
            - prints a STATUS line naming the generated component
        the loop text is not in IDF's app_startup.c
            - configure fails with FATAL_ERROR naming IDF's app_startup.c
        reconfigure with an unchanged IDF
            - app_startup.c keeps its mtime (not rewritten)
        IDF removed a file the generated directory links to
            - the dangling symlink is removed
        IDF added a file
            - the new file is linked
p4 firmware (on board, Task 3)
    first boot after a USB flash on a P4 v3.2
        - boots to the app without a panic
    OTA to a new image on a P4 v3.2
        - the first boot of the new image does not panic
        - the update is kept (running slot is the new slot, app.version is the new build)
    boot of the patched image on a P4 rev1.3
        - boots to the app; /device/status answers
```

## Files

- Create `tools/freertos_startup_patch.cmake`: the generator.
- Create `tools/tests/test_freertos_startup_patch.py`: host tests against a fake `IDF_PATH` tree.
- Modify `p4/CMakeLists.txt`: one include, with a comment.
- Modify `p4/components/PATCHES.md`: an entry under "Generated ESP-IDF copies" that gives the file, the replaced text,
  the reason (issue 19369 and our numbers), and when to drop the patch (once IDF fixes the loop).
- Modify `STYLEGUIDE.md`, section "Build mechanisms": add the new script to the include order.
- Create `docs/known-issues.md`: a guide page.
- Modify `docs/README.md` and `mkdocs.yml` (nav) to list the page, and `docs/troubleshooting.md`, whose limitations
  section links to it.
- Modify `CHANGELOG.md`: a "Fixed" entry under `[Unreleased]`.

---

### Task 1: Generated freertos component

**Interfaces:**
- Consumes: `$ENV{IDF_PATH}`, `CMAKE_BINARY_DIR`, and the `EXTRA_COMPONENT_DIRS` list that `ot_radio_port.cmake` has
  already appended to.
- Produces: `${CMAKE_BINARY_DIR}/idf_patch_components/freertos`. The helpers are local to the file, prefixed
  `freertos_patch_`, and not shared with `ot_radio_port.cmake`. Sharing them would mean refactoring a working script,
  which is out of scope.

- [ ] **Step 1: Write the failing tests** in `tools/tests/test_freertos_startup_patch.py`.
  - A fixture builds a fake IDF tree under `tmp_path`: `components/freertos/app_startup.c` with a few includes and the
    v6.1 loop verbatim, plus `CMakeLists.txt`, `Kconfig` and `FreeRTOS-Kernel/`.
  - A throwaway CMake project sets `IDF_PATH` to that tree, includes the script, and then prints
    `EXTRA_COMPONENT_DIRS`.
  - One test per effect in the `configure` part of the skeleton, named `test_<scenario>_<effect>`.
  - The tests are skipped when `cmake` is missing, as in `test_target_cmake.py`.
- [ ] **Step 2: Run the tests and check that they fail** (the script does not exist yet):
  `set -o pipefail; uv run --with pytest pytest tools/tests/test_freertos_startup_patch.py -v 2>&1 | tee <scratchpad>/t1-red.log`
- [ ] **Step 3: Write `tools/freertos_startup_patch.cmake`** to the Global Constraints, with the same link, patch and
  write logic as `ot_radio_port.cmake`. Its header comment says what is patched and why, links issue 19369, and says
  to drop the script once IDF fixes the loop.
- [ ] **Step 4: Include the script from `p4/CMakeLists.txt`** after `ot_radio_port.cmake`, with a one-line comment.
- [ ] **Step 5: Run the tests again and check that they pass.** Use the same command with the log `t1-green.log`, then
  run `make test-tools`.
- [ ] **Step 6: Build every target:**
  `set -o pipefail; make build-release 2>&1 | tee <scratchpad>/t1-release.log`. Check that:
  - the log has the STATUS line;
  - in `compile_commands.json` of `p4/build_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0`, `app_startup.c` is compiled from
    `idf_patch_components/freertos`.
- [ ] **Step 7: Write the `PATCHES.md` entry and the `STYLEGUIDE.md` line.** Run `make lint`, then commit with
  `fix(p4): pause in CPU1's FreeRTOS start wait (IDF 19369, rev3 first-boot panic)`.

### Task 2: Known-issues page and changelog

- [ ] **Step 1: Write `docs/known-issues.md`, a guide.** It opens with one paragraph: the page lists problems whose
  cause lies outside this project, what they do, and what to do about them. Its first entry is "P4 v3.x boards: the
  first boot after an update panics (firmware up to 0.7.0)":
  - **Who is affected:** boards with a P4 v3.x chip that run the `ESP32-P4-rev3.0` images of releases up to 0.7.0.
  - **What you see:** the first boot after any flash write panics. That covers an update from the page or from GitHub,
    and a USB flash. The board then restarts by itself.
    - After an update it comes back on the old version: the bootloader rolls the update back, and the page only shows
      that the version did not change.
    - After a USB flash, the second boot works.
  - **Cause:** ESP-IDF issue 19369 (linked).
  - **Fixed in:** the next release. The fix is inside the new image, so an update from an affected version to a fixed
    one works.
  - **On affected versions:** flash over USB (the board works after its automatic restart), or try the update again.
    Every attempt that panics is rolled back.
  - **When this entry can go:** once ESP-IDF fixes the loop. The repository's patch is
    `tools/freertos_startup_patch.cmake`.
- [ ] **Step 2: Link the page:**
  - in `docs/README.md` under "Using the board", after Troubleshooting;
  - in the `mkdocs.yml` nav;
  - from the limitations section of `docs/troubleshooting.md`.
- [ ] **Step 3: Add a `CHANGELOG.md` "Fixed" entry under `[Unreleased]`.** It says that rev3 boards no longer panic on
  the first boot after an update or flash, that before this fix an update on these boards was rolled back, and links
  the known-issues page.
- [ ] **Step 4: Check and commit.** Build the site with
  `set -o pipefail; make docs 2>&1 | tee <scratchpad>/t2-docs.log` and expect no warnings about broken links. Run
  `make lint`, then commit with `docs: known issues page with the rev3 first-boot panic`.

### Task 3: On-board pass

This runs after review and fixes. Lease each board through clerk, and re-read `app.version` before every step.

- [ ] **POE `p4-otbr-b` (P4 v3.2), with the `ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0` image.** It is currently
  plugged into the NANO-DB as a test device, so the user has to move it back to the host first.
  - USB flash the image and watch the first boot on the console (`stty -hupcl`). Expected: no panic.
  - Run two OTAs in a row, with builds that have different stamps. After each, watch the first boot, then check the
    running slot and `app.version`. Expected: no panic, and the update is kept.
- [ ] **Rev1.3 board `p4-otbr-a`:** OTA the rev1.3 image. Expected: it boots, `/device/status` answers, and it reports
  the new version.
- [ ] **Record the results** in the memory note `p4-rev3-first-boot-panic` and in the PR description.

### Task 4: PR

- [ ] **Push and open the PR,** only with the user's OK: push `fix/p4-first-boot-panic` and open a PR to `master`.
- [ ] **Rebase the feature branch:** once the PR is merged, rebase `feat/esp32-p4-board-adaptation-7ba687` onto
  master.
