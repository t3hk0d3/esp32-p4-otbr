# P4 rev3 cache livelock (first-boot panic) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Revision 2 (2026-11-25).** Revision 1 shipped a 10 µs pause in CPU1's FreeRTOS start wait, in commits
`3cf52eb4..564abac3` on this branch. Since then Espressif confirmed the cause in
[esp-idf#19369](https://github.com/espressif/esp-idf/issues/19369), and this revision replaces that patch with
Espressif's workaround.

**Goal:** P4 v3.x boards no longer livelock in the cache. Symptom: the first boot after a flash write panics, and an
OTA update is rolled back. The known-issues page states the confirmed cause.

**Architecture:**
- **The cause** (Espressif, #19369, 2026-11-18 and 2026-11-20):
  - `l1_dcache_undef_op` set to 1, 2 or 3 selects random arbitration. That mode has a hardware livelock on every
    P4: when one core keeps loading the same cacheable address (a spin loop), a cache miss on the other core can
    starve.
  - IDF v6.1's `cache_hal_init()` (`components/hal/cache_hal.c`) writes 2 into every cache's `undef_op` field. The
    setter `cache_ll_preload_set_strategy` does this only when `CHIP_SUPPORT_MIN_REV >= 300`, so only our
    `ESP32-P4-rev3.0` images hit it.
  - At boot, the spin is CPU1's wait in `esp_startup_start_app_other_cores`. At runtime, any spin loop can trigger
    it; the tick interrupt usually breaks it there.
- **The workaround** (Espressif): `l1_dcache_undef_op = 0` (round-robin). The L1 I-cache and the L2 cache may stay
  on 2. It costs some performance when D-cache preload is on. Espressif will add an errata entry, and an IDF fix
  is still open.
- **Where to set it:** the app calls `cache_hal_init()` in `esp_system/port/cpu_start.c`, before
  `start_other_core()`. A patched `cache_hal_init` therefore switches the L1 D-cache back before CPU1 runs.
- **How:** "Generated ESP-IDF copies" again. A configure-time script generates a `hal` component in the build
  directory: symlinks to IDF's files, plus a patched `cache_hal.c`.
  - The bootloader is a separate subproject and keeps IDF's `hal`. That is harmless: only CPU0 runs there, and the
    app's `cache_hal_init` writes the register again.
  - The revision 1 freertos script, its tests and its include are removed. One patch that fixes the cause beats two.
    On a third-party board the pause alone left 13 of 20 boots faulting, against 0 of 20 with round-robin.

**Tech Stack:** ESP-IDF v6.1, CMake, pytest (host test of the CMake script), MkDocs.

**Spec:** this plan and the comments on esp-idf#19369 (Espressif's statements of 2026-11-18 and 2026-11-20, and
the A/B data from other users). There is no separate design doc.

**Styleguide:** `STYLEGUIDE.md` (`make lint`). The CMake script follows `tools/ot_radio_port.cmake` and the existing
revision 1 script: a header comment, `<prefix>_link_all` / `<prefix>_replace` / `<prefix>_write` helpers, and a
`FATAL_ERROR` that names the IDF file. A host test of a CMake script uses a throwaway CMake project. Every IDF patch
gets an entry in `p4/components/PATCHES.md`.

## Global Constraints

- **Patched text:** IDF v6.1 `cache_hal.c`, the line
  `cache_ll_preload_set_strategy(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_ARBITRARY);`.
  It is kept as is. Right after it comes a new line,
  `cache_ll_preload_set_strategy(1, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_UNTIL_FETCH_DONE);`, with a
  comment that cites esp-idf#19369. The level argument is `1` because the P4 setter compares `cache_level == 1`.
  The new line writes only `l1_dcache_undef_op = 0`.
- **Generated component:** `${CMAKE_BINARY_DIR}/idf_patch_components/hal`, appended to `EXTRA_COMPONENT_DIRS`.
- **Script:** `tools/cache_arbitration_patch.cmake`, included from `p4/CMakeLists.txt` in place of
  `tools/freertos_startup_patch.cmake`. Its helpers are prefixed `cache_patch_`. Its message texts start with
  `cache_arbitration_patch:`.
- **Scope:** every P4 target. On `ESP32-P4-rev1.3` the setter compiles to nothing, so the image behaves as before.
  The C6 projects don't get the patch.
- **Missing text:** if the replaced text is not found, the configure step stops with `FATAL_ERROR`
  "cache_arbitration_patch: a text to replace is not in <IDF file> any more".
- **Removed:** `tools/freertos_startup_patch.cmake`, `tools/tests/test_freertos_startup_patch.py`, and their mentions
  in `p4/CMakeLists.txt`, `STYLEGUIDE.md`, `PATCHES.md` and `docs/development.md`.
- **No flashing inside tasks.** The on-board pass (Task 3) comes after review and fixes.

## Review Focus

1. **The override reaches the real build.** Expected: the main project compiles `cache_hal.c` from
   `idf_patch_components/hal`. Also check that nothing in IDF refers to `components/hal` by an absolute path that
   would bypass the override; that one is an IDF grep. Covered by Task 1 step 6.
2. **The bootloader still builds** with IDF's own `hal` and links as before. Covered by Task 1 step 6, through
   `make build-release`.
3. **An IDF bump that changes the patched line.** Expected: the configure step fails with the file named. Test: the
   missing-text scenario.
4. **A reconfigure with nothing changed.** Expected: `cache_hal.c` is not rewritten. Test: the reconfigure scenario.
5. **The register really is 0 on the board.** Expected: on the board that reproduces (NANO-DB), the control build
   panics and the patched build doesn't. Covered by Task 3.

## BDD skeleton

```
tools/cache_arbitration_patch.cmake
    configure (included from p4/CMakeLists.txt)
        IDF hal has the v6.1 arbitrary-strategy line
            - writes <build>/idf_patch_components/hal/cache_hal.c with the L1 D-cache round-robin line right after it
            - the IDF line itself is kept
            - the written cache_hal.c is otherwise identical to IDF's
            - every other entry of IDF's components/hal is a symlink in the generated directory
            - appends <build>/idf_patch_components to EXTRA_COMPONENT_DIRS
            - prints a STATUS line naming the generated component
        the line is not in IDF's cache_hal.c
            - configure fails with FATAL_ERROR naming IDF's cache_hal.c
        reconfigure with an unchanged IDF
            - cache_hal.c keeps its mtime
        IDF removed a file the generated directory links to
            - the dangling symlink is removed
        IDF added a file
            - the new file is linked
p4 firmware (on board, Task 3, NANO-DB P4 v3.2)
    control build (same tree, patch include commented out), first boot after a USB flash
        - panics (reproduces the bug on this board; if it does not, the pass cannot prove the fix: stop and report)
    patched build, first boot after a USB flash, 3 times
        - no panic; reaches the app
    control build running, OTA to the patched build
        - no panic; the update is kept (running slot and app.version are the patched ones)
    patched build, OTA to a second patched build, twice
        - no panic; the update is kept (running slot and app.version are the new ones)
```

## Files

- Create `tools/cache_arbitration_patch.cmake`.
- Create `tools/tests/test_cache_arbitration_patch.py`.
- Delete `tools/freertos_startup_patch.cmake` and `tools/tests/test_freertos_startup_patch.py`.
- Modify:
  - `p4/CMakeLists.txt`
  - `p4/components/PATCHES.md`: replace the freertos entry under "Generated ESP-IDF copies".
  - `STYLEGUIDE.md`: "Build mechanisms".
  - `docs/development.md`: "Vendored code".
- Modify:
  - `docs/known-issues.md`: cause, scope (first boot, rarely at runtime), what the fix does.
  - `CHANGELOG.md`: the "Fixed" entry.

---

### Task 1: Generated hal component replaces the freertos patch

**Interfaces:**
- **Consumes:** `$ENV{IDF_PATH}`, `CMAKE_BINARY_DIR`, and `EXTRA_COMPONENT_DIRS`.
- **Produces:** `${CMAKE_BINARY_DIR}/idf_patch_components/hal`.

- [ ] **Step 1: Write the failing tests** in `tools/tests/test_cache_arbitration_patch.py`.
  - Use the revision 1 freertos test file as the pattern: a fake IDF tree and a throwaway project.
  - The fake `components/hal/cache_hal.c` holds a few lines around the v6.1 line, verbatim. Next to it go
    `CMakeLists.txt`, `linker.lf` and an `esp32p4/` directory.
  - Write one test per effect in the `configure` part of the skeleton. Name each `test_<scenario>_<effect>`.
  - In every test that reads generated files, first assert that the configure step returned 0. This closes the
    revision 1 deferred minor.
- [ ] **Step 2: Run the tests and check that they fail:**
  `set -o pipefail; uv run --with pytest pytest tools/tests/test_cache_arbitration_patch.py -q 2>&1 | tee <scratchpad>/r2-red.log`
- [ ] **Step 3: Write `tools/cache_arbitration_patch.cmake`** to the Global Constraints. The header comment names:
  - the cause and the issue link;
  - Espressif's statement that only the L1 D-cache field is affected;
  - the performance note;
  - that the bootloader keeps IDF's hal;
  - when to drop the patch: once IDF sets round-robin itself or fixes mode 2.
- [ ] **Step 4: Swap the include in `p4/CMakeLists.txt`,** then delete the freertos script and its test.
- [ ] **Step 5: Run the tests again and check that they pass** (log `r2-green.log`), then run `make test-tools`.
- [ ] **Step 6: Build and check.**
  - Run `grep -rn 'components/hal' $IDF_PATH/components $IDF_PATH/tools/cmake` and check that nothing builds `hal`
    sources by absolute path.
  - Run `set -o pipefail; make build-release 2>&1 | tee <scratchpad>/r2-release.log`.
  - Check the STATUS line in the log.
  - Check that `cache_hal.c` is compiled from `idf_patch_components/hal` in the `compile_commands.json` of both
    master targets.
  - Check that `bootloader/compile_commands.json` still uses IDF's `cache_hal.c`.
  - Check that the rev3 `p4.elf` disassembly of `cache_hal_init` holds two stores to the cache control block. Do this
    with `riscv32-esp-elf-objdump -d` and a look at the function.
- [ ] **Step 7: Update the docs and commit.** Replace the `PATCHES.md` entry, then update `STYLEGUIDE.md` and
  `docs/development.md`. Run `make lint` and check that it reports nothing new. Commit with
  `fix(p4): round-robin L1 D-cache arbitration (IDF 19369 livelock), replacing the start-wait pause`.

### Task 2: Docs

- [ ] **Step 1: Rewrite the cause and fix sections of `docs/known-issues.md`.**
  - **Cause:** a P4 hardware issue that Espressif confirmed. In ESP-IDF v6.1 the L1 data cache uses random
    arbitration on rev3 firmware. When one core keeps waiting in a loop, the other core's memory access can lock
    up, and on the first boot after a write this ends in a panic. Link the issue.
  - **Also at runtime:** the same thing can rarely happen while the board runs, as a crash with a restart. This is
    reported by other users and has not been seen on our boards.
  - **The fix:** the firmware sets the round-robin mode, as Espressif recommends.
  - Fold in the deferred minor: say "v3.1 and later (seen on v3.1 and v3.2)" instead of "v3.x".
  - Keep the rest: who is affected, what you see, what to do.
- [ ] **Step 2: Update the `CHANGELOG.md` "Fixed" entry** to the new cause. It also covers rare runtime crashes.
- [ ] **Step 3:** run `set -o pipefail; make docs 2>&1 | tee <scratchpad>/r2-docs.log` and check that it reports no
  link warnings. Then commit with `docs: known issue names the confirmed cache arbitration cause`.

### Task 3: On-board pass on the NANO-DB (after review and fixes)

The NANO-DB is the board that reproduced the bug (4 of 4 first boots panicked). The POE did not reproduce it, even
on 0.7.0 (3 of 3 clean), so it cannot prove the fix. Master has no NANO-DB target, so the pass uses a local
throwaway branch:

- [ ] **Set up the branch.** Create a local throwaway branch off `feat/esp32-p4-board-adaptation-7ba687` and
  cherry-pick the Task 1 commit onto it. Never push this branch, and don't commit it to the feature branch.
- [ ] **Build the control and the patched images.**
  - The control is the same tree with the include commented out, built with `make build-test` for target
    `ESP32-P4-NANO-WIFI6-DB`/`ESP32-P4-rev3.0`.
  - The patched images are that tree with the include, built twice with different stamps.
  - Build every image in a fresh build directory (`rm -rf` it first), so no generated component of an earlier build
    is left in it.
- [ ] **Lease and check the board.** Lease `p4-nano-db/nano-db-a` through clerk, and read `app.version` before every
  step.
- [ ] **Control, one USB flash.** Run `usbflash_boot.sh` with a 40 s console capture. Expected: a panic. If the board
  boots cleanly, stop and report.
- [ ] **Patched build, three USB flashes.** Expected: no panic, and the board reaches the OpenThread main loop.
- [ ] **Affected to fixed, one OTA.** With the control build running (after its automatic restart), OTA the first
  patched image. This is the path `docs/known-issues.md` promises users on 0.6.0/0.7.0. Expected: no panic on the
  patched image's first boot, and the update is kept (running slot and `app.version` are the patched ones).
- [ ] **Patched build, two OTAs** of the other patched stamp and back. After each, watch the first boot, then check
  the running slot and `app.version`. Expected: no panic, and the update is kept.
- [ ] **Leave the board as the next user expects.** Put the NANO-DB back on a feature-branch build. Release the lease
  with a note.
- [ ] **Record the results** in the memory note `p4-rev3-first-boot-panic` and in the PR description.

### Task 4: PR

- [ ] **Open the PR** only with the user's OK: push `fix/p4-first-boot-panic` and open a PR to `master`. The
  description gives the cause, Espressif's statement, our A/B numbers, and the performance note.
- [ ] **After the merge,** rebase `feat/esp32-p4-board-adaptation-7ba687` onto master.
