# Flash in the browser

Write the P4 firmware of the [latest release](https://github.com/t3hk0d3/esp32-p4-otbr/releases/latest) from this
page, without installing anything. It needs Chrome, Edge or another browser with Web Serial, on a desktop.

1. Connect the board's P4 USB-C port to this computer (the same port as for `make flash`).
2. Press **Install**, pick the board's serial port (a CH343, often named "USB Single Serial") and confirm.
3. When it is done, continue with the radio firmware: [First install](first-install.md), from Path 1, 2 or 3 after
   "Flash the P4".

<script type="module" src="https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"></script>

<esp-web-install-button manifest="../firmware/manifest.json">
  <span slot="unsupported">This browser can't flash: open the page in Chrome or Edge on a desktop.</span>
  <span slot="not-allowed">Flashing needs this page over HTTPS.</span>
</esp-web-install-button>

<p id="flash-version"></p>

<script>
  fetch("../firmware/manifest.json")
    .then((r) => (r.ok ? r.json() : Promise.reject()))
    .then((m) => (document.getElementById("flash-version").textContent = `Release: ${m.version}`))
    .catch(() => (document.getElementById("flash-version").textContent = "No firmware on this copy of the site."));
</script>

It writes the same images as `p4-flash.zip` (bootloader, partition table, OTA data, app, web UI) at the offsets of its
`flash_args`. The installer offers to erase the flash first: do that on a new board or one that ran other firmware.
On a board in service leave it off, since erasing also deletes the stored Thread dataset and network configuration.
The C6 radio is not touched; upload `c6_installer.bin` (factory C6 only) and `c6_rcp.bin` from the same release on the
board's Firmware page.
