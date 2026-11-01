# Sniffer

Stream an IEEE 802.15.4 capture straight into Wireshark:

```
curl -N "http://<board>/device/sniffer.pcap?channel=15" | wireshark -k -i -
```

`channel` is 11..26 and defaults to the channel of the active dataset (400 `no_channel` if there is no dataset).
`duration` (seconds) defaults to 600, maximum 3600. Only in `BR_RUNNING` (else 409 `busy`). The capture ends on
the duration, when the client disconnects, or on `curl -X DELETE http://<board>/device/sniffer`, and the border router
resumes by itself. The device page can start, stop and download a capture too.

- While sniffing, the Thread stack is stopped: the Thread REST API answers 503 for reads and 409 for changes, and the
  Thread network sees the border router leave. Ethernet (IPv4/IPv6, the web UI and `/device/*`) keeps working.
- The format is pcap with link type 283 (IEEE 802.15.4 TAP). The radio delivers frames without a valid FCS
  (RSSI and LQI replace the last two bytes), so the capture carries no FCS: the TAP header says "no FCS" and has
  RSSI, LQI and channel fields. Wireshark's FCS-related checks do not apply.
- Frames come through a 256 KB ring; when the client reads too slowly frames are dropped and counted
  (`sniffer.drops` in `/device/status`).
- A sniffer session is never persisted: a reboot returns to the border router.
