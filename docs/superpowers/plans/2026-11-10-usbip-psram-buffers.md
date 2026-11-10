# USB/IP transfer buffers in PSRAM, 120 KB transfers

**Goal:** USB flash drives work over USB/IP with no client setup, and USB/IP stops costing internal RAM for transfer
buffers.

**Why:** the hardware pass of 2026-11-10 showed Linux `usb-storage` reads of up to 120 KB (`max_sectors` 240). The
16 KB `USBIP_XFER_MAX` refuses the first one, the client's port reset is only acknowledged (spec §6.4 ruling), and the
drive stays stuck until a kick. With `max_sectors_kb=16` set by hand it worked at 2.7 MB/s; the user is fine with that
speed and wants the limit raised with the buffers in PSRAM (2026-11-10).

**Spec:** `docs/superpowers/specs/2026-11-10-usbip-design.md` §8.1 (this plan reverses its "DMA buffers stay in
internal RAM" call; the section is rewritten in Task 3).

**Style:** follow `STYLEGUIDE.md` (naming, `ESP_RETURN_ON_*` error handling, host tests in `test_host/` per
component, Unity `TEST_CASE("On <scenario>: <effect>")`). Commands run as `<cmd> 2>&1 | tee <scratchpad log>`.

## Facts this rests on (checked in esp-usb 1.6.0)

- `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` (P4, needs `SPIRAM`): esp-usb allocates transfer data buffers
  (`urb_alloc`) and transfer descriptor lists with `MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM`,
  and cache-syncs them around each transfer. The periodic frame list stays internal.
- `urb_alloc` already rounds the buffer size up to the cache line (`ALIGN_UP(size, cache_align)`), so the Kconfig
  TODO IDF-11368 (aligned allocation size) does not bite buffers made by `usb_host_transfer_alloc`, which is the only
  way `usb_side.c` allocates DMA memory.
- Per-transfer hardware limit (`hcd_pipe_get_xfer_size_limit`): 131071 bytes floored to whole packets: 130560 for a
  512-byte bulk endpoint, 130048 for a 1024-byte high-speed interrupt endpoint. 120 KB = 122880 fits every endpoint.
- The option is global: the USB serial radio channels' buffers (CH34x, CP210x, CDC drivers) move to PSRAM too.

## Decisions

| Item | Now | New | Why |
| --- | --- | --- | --- |
| `USBIP_XFER_MAX` | 16 KB | 120 KB (122880) | Linux `usb-storage` default; under the hardware limit of every endpoint |
| `USBIP_INFLIGHT_BYTES` (IN shared) | 32 KB | 256 KB | two 120 KB reads plus small ones; PSRAM has ~30 MB free |
| `USBIP_INFLIGHT_HARD` | 80 KB literal | expression: shared + `USBIP_XFER_MAX` + (`USBIP_EP_MAX` + 1) x `USBIP_RESERVE_SMALL` = 408 KB | keeps the §8.1 invariant "shared and small grants leave room for one large transfer" by construction |
| Heap floor | 48 KB internal DMA (`USBIP_DMA_FREE_FLOOR`) | 1 MB PSRAM DMA (`USBIP_PSRAM_FREE_FLOOR`, checked against `MALLOC_CAP_DMA \| MALLOC_CAP_SPIRAM`) | the buffers now come from PSRAM; 1 MB leaves room for OTA and the other PSRAM users |
| `TX_RING_LEN` (net.c) | 256 KB | 768 KB | must hold every IN completion that can be in flight (408 KB + headers) so a burst never reads as "client does not read" |
| `USBIP_WAIT_BYTES` | 1 MB | unchanged | ≥ 8 waiting 120 KB transfers |
| Kconfig | off | `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y` in `p4/sdkconfig.defaults` | the user's call |

PSRAM while a session runs, worst case: 768 KB ring + 120 KB receive buffer + 408 KB in flight + 1 MB waiting ≈ 2.3 MB.

Compile-time checks (`_Static_assert`): `USBIP_XFER_MAX <= 130048`; `TX_RING_LEN >= USBIP_INFLIGHT_HARD +
USBIP_USB_XFERS_MAX * USBIP_CMD_HDR_LEN`; `USBIP_WAIT_BYTES >= USBIP_XFER_MAX`.

## BDD skeleton

```
usbip_core_check_submit (core.c)
    On a bulk SUBMIT of exactly USBIP_XFER_MAX (120 KB)
        - returns 0
    On a bulk SUBMIT of USBIP_XFER_MAX + 1
        - returns -EINVAL
    On a control SUBMIT over USBIP_CTRL_DATA_MAX (4 KB)
        - returns -EINVAL (unchanged)

usbip_budget_grant (budget.c)
    On two 120 KB IN reads on one endpoint
        - grants both (shared 256 KB)
    On shared IN bytes full and a 120 KB OUT on an idle endpoint
        - grants the OUT (large reserve)
    On a second large transfer while one is in flight and shared is full
        - refuses it (waits in its FIFO)
    On every endpoint's small reserve plus full shared plus one large
        - in-flight bytes stay <= USBIP_INFLIGHT_HARD (408 KB)

usbip_core_alloc_fits(size_t free, size_t n, size_t floor) -> bool (core.c, new pure helper; usb_side.c calls it)
    On free - n >= floor
        - returns true
    On free - n < floor
        - returns false
    On n > free
        - returns false (no underflow)

ops_alloc (usb_side.c, target only)
    On a DMA buffer request
        - checks free PSRAM DMA memory against USBIP_PSRAM_FREE_FLOOR via usbip_core_alloc_fits
        - allocates through usb_host_transfer_alloc (PSRAM with the Kconfig on)
    On the floor not met
        - returns NULL (the client gets -ENOMEM), logs the numbers

Board (hardware pass, Task 4)
    On attaching the JetFlash with no client setup
        - partition table, blkid probe and read-only mount succeed; no I/O errors, no resets in dmesg
        - dd read of 64 MB completes; speed recorded
    On a session with traffic
        - internal free heap is not lower than with USB/IP off; PSRAM in-flight peak <= 408 KB
    On the OllyZig as a USB radio channel (rcp-proxy, USB/IP off)
        - spinel version probe answers 200/200 through the proxy
    On the CH340 OllyZig exported again
        - the RTT probe from the first pass still answers 1000/1000
```

## Tasks

### Task 1: limits, budget and floor

- Modify `p4/components/usbip/include/usbip_core.h`: the constants above; declare `usbip_core_alloc_fits`.
- Modify `p4/components/usbip/core.c`: implement `usbip_core_alloc_fits`.
- Modify `p4/components/usbip/include/usbip_budget.h`: the header comment's numbers and the internal-RAM paragraph
  (now PSRAM).
- Modify `p4/components/usbip/usbip_priv.h`: `USBIP_DMA_FREE_FLOOR` → `USBIP_PSRAM_FREE_FLOOR` (1 MB).
- Modify `p4/components/usbip/usb_side.c` `ops_alloc`: PSRAM DMA free size and the helper; update the comment that
  says the Kconfig is off.
- Modify `p4/components/usbip/net.c`: `TX_RING_LEN` 768 KB and the static asserts.
- Modify `p4/sdkconfig.defaults`: add the Kconfig line next to `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`.
- Tests: `p4/components/usbip/test_host/main/test_core.c` and `test_budget.c`, one test per effect above; existing
  budget tests already use the constants, fix any that hard-code 16/32/80 KB.
- Verify: `make test-host`, `make build-test`; record internal and PSRAM static sizes (`make sizes`) against the
  current head.
- Commit: `feat(usbip): transfer buffers in PSRAM, 120 KB transfers`.

### Task 2: other USB users

- Read the radio channel USB drivers (`usb_host_ch34x_vcp`, `usb_host_cp210x_vcp`, CDC) and `radio_mux`'s USB
  transport for any buffer they allocate themselves with internal-only caps and hand to esp-usb; with the Kconfig on
  such a buffer must still be DMA-capable for the controller. Expected: none (they use `usb_host_transfer_alloc`).
  If one exists, it gets the same caps as `urb_alloc`.
- Verify: `make build-test`, `make test-host`.
- Commit only if something changed.

### Task 3: docs and spec

- `docs/superpowers/specs/2026-11-10-usbip-design.md` §8.1: buffers in PSRAM (user's call 2026-11-10, reverses the
  earlier call), the new numbers, the floor; the grant pseudocode's 32 KB / 80 KB become the constant names.
- `docs/usbip.md`: Limits (120 KB), Memory paragraph (PSRAM, numbers), "USB flash drives" (no client setup needed;
  drop the udev rule; keep the port-reset limit for other devices).
- `CHANGELOG.md`: one line under the USB/IP entry.
- Verify: `make docs SITE=<scratchpad>/site` builds without warnings.
- Commit: `docs(usbip): PSRAM buffers, 120 KB transfers`.

### Task 4: hardware pass (one P4 OTA, no C6 writes)

Lease `p4-otbr-boards` first. OTA the test build, then run the Board scenarios above with the Docker client
(`usbip-client:local`) and the JetFlash read-only, then swap to the OllyZig for the two radio checks (needs the user
to swap the plug). Restore: USB/IP off, release the lease, note the build in the clerk unit. Record results in memory
(`usbip-server.md`).

## Review focus

- A 120 KB IN completion arriving while the TX ring holds other replies: the ring size must cover it (static assert).
- The PSRAM floor during an OTA (the OTA writer also uses PSRAM).
- Cache sync cost on the USB serial radio channels: frame latency of the OllyZig in rcp-proxy mode.
- `waiting` accounting with 120 KB OUT data in the FIFOs (1 MB cap = 8 such writes).
- Nothing else in the firmware assumed USB DMA buffers are internal (search for `MALLOC_CAP_INTERNAL` near USB code).
