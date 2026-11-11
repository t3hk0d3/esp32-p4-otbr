"""docs/assets/flasher.js and docs/overrides/flash.html: the browser flasher, under node with fakes (flash_page_harness.js).

A test checks one effect of the behaviour contract; a class is one scenario.
"""

import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest
from device_js import REPO, needs_node

pytestmark = needs_node

HARNESS = Path(__file__).resolve().parent / "flash_page_harness.js"
TEMPLATE = REPO / "docs" / "overrides" / "flash.html"
BOARD = "ESP32-P4-WIFI6-POE-ETH"
V1 = "ESP32-P4-rev1.3"
V3 = "ESP32-P4-rev3.0"

INIT = "await t.flow.init();"
DETECT = INIT + "await t.flow.detect();"
INSTALL = DETECT + "await t.flow.install();"
INSTALL_KEEP = DETECT + "t.flow.setErase(false); await t.flow.install();"
INSTALL_ERASE = DETECT + "t.flow.setErase(true); await t.flow.install();"
BY_HAND = (
    INIT
    + f"t.flow.chooseByHand(); t.flow.pickChip('{V3}'); t.flow.setErase(false); await t.flow.install();"
)


def node(source):
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=30
    )
    return json.loads(out.stdout) if out.stdout else None


def run(body, **options):
    """Run `body` with t = a fresh flow with these fake options; return t.result()."""
    return node(
        f"const H = require({json.dumps(str(HARNESS))});"
        f"const t = H.makeFlow({json.dumps(options)});"
        f"(async () => {{ {body} process.stdout.write(JSON.stringify(t.result())); }})()"
        ".catch(e => { console.error(e); process.exit(1); });"
    )


def run_several_boards(body, **options):
    """As run, publishing a second board for the v1 chip."""
    return node(
        f"const H = require({json.dumps(str(HARNESS))});"
        f"const t = H.makeFlow(Object.assign({json.dumps(options)}, {{targets: H.SEVERAL_BOARDS}}));"
        f"(async () => {{ {body} process.stdout.write(JSON.stringify(t.result())); }})()"
        ".catch(e => { console.error(e); process.exit(1); });"
    )


def logic(expression):
    return node(
        f"const H = require({json.dumps(str(HARNESS))}); const F = H.Flasher;"
        f"process.stdout.write(JSON.stringify({expression}));"
    )


def template():
    return TEMPLATE.read_text()


def panel(stage):
    """The markup of the section shown in `stage`."""
    for match in re.finditer(
        r'<section data-stage="([^"]*)"[^>]*>(.*?)</section>', template(), re.DOTALL
    ):
        if stage in match.group(1).split():
            return match.group(2)
    raise AssertionError(f"no panel for {stage}")


def actions(stage):
    return re.findall(r'data-action="(\w+)"', panel(stage))


def text_of(stage):
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", panel(stage)))


def released(result):
    return "disconnect" in result["events"] and not result["portOpen"]


def assert_reset_then_release(events):
    flashing = [
        e for e in events if not e.startswith("console-") and e != "sleep:60000"
    ]
    tail = flashing[-5:]
    assert tail == ["dtr:false", "rts:true", "sleep:100", "after", "disconnect"]


class TestTargetsForRevision:
    def test_inside_a_range_returns_the_targets_of_that_chip(self):
        found = logic("F.targetsForRevision(H.SEVERAL_BOARDS, 103)")
        assert [(t["board"], t["chip"]) for t in found] == [
            (BOARD, V1),
            ("Second-Board", V1),
        ]

    def test_at_the_ends_of_a_range_returns_it(self):
        assert [
            len(logic(f"F.targetsForRevision(H.TARGETS, {r})"))
            for r in (0, 199, 301, 399)
        ] == [1, 1, 1, 1]

    @pytest.mark.parametrize("revision", [200, 300, 400])
    def test_no_target_covers_it_returns_an_empty_list(self, revision):
        assert logic(f"F.targetsForRevision(H.TARGETS, {revision})") == []


class TestHostFromMac:
    def test_uses_the_last_three_bytes(self):
        assert logic("F.hostFromMac('aa:bb:cc:dd:ee:ff')") == "esp-otbr-ddeeff.local"


class TestDetectOneBoard:
    def test_shows_the_chip_board_and_release(self):
        view = run(DETECT)["view"]
        assert view["stage"] == "detected"
        assert view["rev"] == "v1.3"
        assert view["chipId"] == V1
        assert view["boardName"] == f"Waveshare {BOARD}"
        assert view["release"] == "Release v9.9.9"

    def test_offers_install(self):
        assert run(DETECT)["view"]["canInstall"] is True
        assert "install" in actions("detected")

    def test_picks_the_v3_target_for_a_v3_chip(self):
        assert run(DETECT, revision=302)["view"]["chipId"] == V3

    def test_keeps_the_serial_port_open(self):
        result = run(DETECT)
        assert result["portOpen"] is True
        assert "disconnect" not in result["events"]


class TestDetectSeveralBoards:
    def test_asks_which_board(self):
        view = run_several_boards(DETECT)["view"]
        assert view["showBoardChoices"] is True
        assert [c["value"] for c in view["boardChoices"]] == [BOARD, "Second-Board"]

    def test_offers_no_install_before_a_pick(self):
        assert run_several_boards(DETECT)["view"]["canInstall"] is False

    def test_offers_install_once_one_is_picked(self):
        view = run_several_boards(DETECT + "t.flow.pickBoard('Second-Board');")["view"]
        assert view["canInstall"] is True
        assert view["boardName"] == "Second board"


class TestDetectUncoveredRevision:
    def result(self):
        return run(DETECT, revision=250)

    def test_names_the_revision(self):
        view = self.result()["view"]
        assert view["stage"] == "no_firmware"
        assert view["rev"] == "v2.50"
        assert 'data-text="rev"' in panel("no_firmware")

    def test_offers_no_install(self):
        assert self.result()["view"]["canInstall"] is False
        assert "install" not in actions("no_firmware")

    def test_resets_the_board(self):
        assert_reset_then_release(self.result()["events"])

    def test_releases_the_port(self):
        assert released(self.result())


class TestDetectOtherChip:
    def result(self):
        return run(DETECT, chipName="ESP32-C6")

    def test_says_so(self):
        view = self.result()["view"]
        assert view["stage"] == "wrong_chip"
        assert view["chipName"] == "ESP32-C6"
        assert "not an ESP32-P4" in text_of("wrong_chip")

    def test_offers_no_install(self):
        assert self.result()["view"]["canInstall"] is False
        assert "install" not in actions("wrong_chip")

    def test_does_not_read_the_revision(self):
        assert "revision" not in self.result()["events"]

    def test_resets_the_board(self):
        assert_reset_then_release(self.result()["events"])

    def test_releases_the_port(self):
        assert released(self.result())


class TestDetectPickerCancelled:
    def test_changes_nothing(self):
        result = run(DETECT, cancelPicker=True)
        assert result["view"]["stage"] == "idle"
        assert "transport" not in result["events"]

    def test_returns_to_a_failure_it_started_from(self):
        body = INIT + "t.flow.state.stage = 'connect_failed'; await t.flow.detect();"
        assert run(body, cancelPicker=True)["view"]["stage"] == "connect_failed"


class TestDetectConnectionFails:
    def test_says_to_hold_boot_and_press_reset(self):
        assert run(DETECT, failConnect=True)["view"]["stage"] == "connect_failed"
        words = text_of("connect_failed")
        assert "hold BOOT" in words
        assert "RESET" in words
        assert "try again" in words

    def test_offers_trying_again_or_choosing_by_hand(self):
        assert actions("connect_failed") == ["detect", "chooseByHand"]

    def test_releases_the_serial_port(self):
        assert released(run(DETECT, failConnect=True))

    def test_survives_a_failing_release(self):
        result = run(DETECT, failConnect=True, failDisconnect=True)
        assert result["view"]["stage"] == "connect_failed"


class TestChooseByHand:
    def test_the_v3_choice_says_v3_1_and_later(self):
        # the label is the registry's chip name: v3.0 gets no firmware
        registry = json.loads((REPO / "p4" / "targets" / "targets.json").read_text())
        name = registry["chips"][V3]["name"]
        assert "v3.1 and later" in name
        assert "v3.x" not in name

    def test_offers_no_install_before_a_chip_is_picked(self):
        assert run(INIT + "t.flow.chooseByHand();")["view"]["canInstall"] is False

    def test_install_asks_for_the_port(self):
        assert "requestPort" in run(BY_HAND)["events"]

    def test_install_writes_that_target(self):
        events = run(BY_HAND)["events"]
        assert f"fetch:fw/{BOARD}/{V3}/manifest.json" in events
        assert "write" in events

    def test_install_does_not_read_the_revision(self):
        assert "revision" not in run(BY_HAND)["events"]

    def test_refuses_a_chip_that_is_not_an_esp32_p4(self):
        result = run(BY_HAND, chipName="ESP32-C6")
        assert result["view"]["stage"] == "wrong_chip"
        assert "write" not in result["events"]

    def test_lists_the_published_chips(self):
        view = run(INIT + "t.flow.chooseByHand();")["view"]
        assert [c["value"] for c in view["chipChoices"]] == [V1, V3]

    def test_cancelled_picker_returns_to_the_choice(self):
        result = run(BY_HAND, cancelPicker=True)
        assert result["view"]["stage"] == "manual"


class TestInstallSuccess:
    def test_writes_each_part_at_its_offset(self):
        assert run(INSTALL)["written"]["files"] == [
            {"address": 0x2000, "size": 100},
            {"address": 0x100000, "size": 300},
        ]

    def test_keeps_the_flash_settings(self):
        written = run(INSTALL)["written"]
        assert (written["flashMode"], written["flashFreq"], written["flashSize"]) == (
            "keep",
            "keep",
            "keep",
        )

    def test_verifies_each_part_by_md5(self):
        assert run(INSTALL)["written"]["md5"] == ["md5-of-100", "md5-of-300"]

    def test_shows_overall_progress(self):
        percents = [p["percent"] for p in run(INSTALL)["progress"]]
        assert percents == [0, 12, 25, 25, 62, 100]

    def test_shows_which_part_is_being_written(self):
        parts = [p["part"] for p in run(INSTALL)["progress"]]
        assert parts[0] == "the bootloader"
        assert parts[-1] == "the border router"

    def test_resets_the_board(self):
        assert_reset_then_release(run(INSTALL)["events"])

    def test_releases_the_port(self):
        assert released(run(INSTALL))

    def test_links_to_the_boards_page(self):
        view = run(INSTALL)["view"]
        assert view["stage"] == "done"
        assert view["host"] == "esp-otbr-ddeeff.local"
        assert view["hostUrl"] == "http://esp-otbr-ddeeff.local/"
        assert 'data-href="hostUrl"' in panel("done")

    def test_says_which_release_it_installed(self):
        assert run(INSTALL)["view"]["version"] == "v9.9.9"
        assert 'data-text="version"' in panel("done")

    def test_survives_a_failing_release(self):
        assert run(INSTALL, failDisconnect=True)["view"]["stage"] == "done"


GOT_IP = "\x1b[0;32mI (4210) eth: got ip 192.168.1.50\x1b[0m\r\n"
UNTIL_DONE = (
    "while (t.flow.view().stage !== 'done') await new Promise(r => setImmediate(r));"
    "await new Promise(r => setImmediate(r));"
)


class TestBoardAddress:
    def test_links_to_the_address_the_board_printed(self):
        view = run(INSTALL, console=["boot\r\n", GOT_IP])["view"]
        assert view["hostUrl"] == "http://192.168.1.50/"
        assert view["openLabel"] == "Open 192.168.1.50"
        assert view["canOpen"] is True
        assert view["addressWaiting"] is False

    def test_finds_an_address_split_across_reads(self):
        view = run(INSTALL, console=["I (1) eth: got i", "p 10.0.0.7\r\n"])["view"]
        assert view["hostUrl"] == "http://10.0.0.7/"

    def test_ignores_ipv6_addresses(self):
        view = run(INSTALL, console=["I (1) eth: got ip6 fe80::1 link-local\r\n"])[
            "view"
        ]
        assert view["hostUrl"] == "http://esp-otbr-ddeeff.local/"

    def test_reads_the_console_at_its_own_speed_after_releasing_the_flasher(self):
        events = run(INSTALL, console=[GOT_IP])["events"]
        assert events.index("disconnect") < events.index("console-open:115200")

    def test_closes_the_port_once_found(self):
        result = run(INSTALL, console=[GOT_IP])
        assert "console-close" in result["events"]
        assert not result["portOpen"]

    def test_falls_back_to_the_name_when_nothing_is_printed_in_time(self):
        result = run(INSTALL)
        assert result["view"]["hostUrl"] == "http://esp-otbr-ddeeff.local/"
        assert result["view"]["openLabel"] == "Open esp-otbr-ddeeff.local"
        assert result["view"]["canOpen"] is True
        assert not result["portOpen"]

    def test_falls_back_to_the_name_when_the_port_will_not_open(self):
        view = run(INSTALL, failConsoleOpen=True)["view"]
        assert view["hostUrl"] == "http://esp-otbr-ddeeff.local/"
        assert view["addressWaiting"] is False

    def test_offers_no_link_while_waiting(self):
        body = DETECT + "const p = t.flow.install();" + UNTIL_DONE
        view = run(body, holdConsole=True)["view"]
        assert view["addressWaiting"] is True
        assert view["canOpen"] is False

    def test_flash_another_board_stops_waiting_and_frees_the_port(self):
        body = (
            DETECT
            + "const p = t.flow.install();"
            + UNTIL_DONE
            + "await t.flow.restart(); await p;"
        )
        result = run(body, holdConsole=True)
        assert result["view"]["stage"] == "idle"
        assert result["view"]["addressWaiting"] is False
        assert "console-close" in result["events"]
        assert not result["portOpen"]

    def test_stops_waiting_when_the_port_never_opens(self):
        body = DETECT + "const p = t.flow.install();" + UNTIL_DONE + UNTIL_DONE
        view = run(body, hangConsoleOpen=True)["view"]
        assert view["addressWaiting"] is False
        assert view["hostUrl"] == "http://esp-otbr-ddeeff.local/"

    def test_shows_the_link_even_if_closing_the_port_hangs(self):
        body = DETECT + "const p = t.flow.install();" + UNTIL_DONE + UNTIL_DONE
        view = run(body, console=[GOT_IP], hangConsoleClose=True)["view"]
        assert view["hostUrl"] == "http://192.168.1.50/"
        assert view["addressWaiting"] is False

    def test_the_link_opens_in_a_new_tab(self):
        link = re.search(r'<a [^>]*data-href="hostUrl"[^>]*>', panel("done")).group(0)
        assert 'target="_blank"' in link
        assert 'rel="noopener"' in link
        assert 'data-if="canOpen"' in link

    def test_says_it_is_waiting_for_the_address(self):
        assert 'data-if="addressWaiting"' in panel("done")

    def test_shows_a_spinner_while_waiting(self):
        note = re.search(
            r'<p [^>]*data-if="addressWaiting".*?</p>', panel("done"), re.DOTALL
        ).group(0)
        assert 'class="fl-spinner"' in note
        assert 'aria-hidden="true"' in note


class TestInstallErase:
    def test_erase_chosen_erases_before_writing(self):
        events = run(INSTALL_ERASE)["events"]
        assert events.index("erase") < events.index("write")

    def test_erase_chosen_shows_the_erasing_stage(self):
        result = run(INSTALL_ERASE)
        assert result["viewWhileErasing"]["stage"] == "erasing"

    def test_erase_not_chosen_does_not_erase(self):
        assert "erase" not in run(INSTALL_KEEP)["events"]

    def test_the_box_starts_unticked(self):
        assert run(DETECT)["view"]["erase"] is False

    def test_the_box_has_no_checked_attribute_in_the_template(self):
        boxes = re.findall(r'<input[^>]*data-check="erase"[^>]*>', template())
        assert len(boxes) == 2
        assert not [box for box in boxes if "checked" in box.replace("data-check", "")]

    def test_install_without_touching_the_box_does_not_erase(self):
        assert "erase" not in run(INSTALL)["events"]

    def test_install_by_hand_without_touching_the_box_does_not_erase(self):
        body = (
            INIT
            + f"t.flow.chooseByHand(); t.flow.pickChip('{V3}'); await t.flow.install();"
        )
        result = run(body)
        assert "write" in result["events"]
        assert "erase" not in result["events"]

    def test_the_label_says_when_to_tick_it(self):
        for stage in ("detected", "manual"):
            assert "Tick this on a new board or one that ran other firmware" in text_of(
                stage
            )


class TestInstallDownloadFails:
    def result(self):
        return run(INSTALL, failDownload="p4.bin")

    def test_says_which_file(self):
        view = self.result()["view"]
        assert view["stage"] == "download_failed"
        assert view["file"] == "p4.bin"
        assert 'data-text="file"' in panel("download_failed")

    def test_stops_before_touching_the_flash(self):
        events = self.result()["events"]
        assert "erase" not in events
        assert "write" not in events

    def test_releases_the_port(self):
        assert released(self.result())

    def test_offers_to_try_again(self):
        assert "install" in actions("download_failed")

    def test_retry_asks_for_the_port_again(self):
        body = INSTALL + "t.events.length = 0; await t.flow.install();"
        assert "requestPort" in run(body, failDownload="p4.bin")["events"]


@pytest.mark.parametrize(
    "failure",
    [
        {"failWrite": "write failed"},
        {"failWrite": "MD5 of file does not match data in flash!"},
        {"failWrite": "The device has been lost.", "failDisconnect": True},
        {"failErase": True},
    ],
    ids=["write", "md5", "cable", "erase"],
)
class TestInstallWriteFails:
    def test_says_the_board_may_not_start(self, failure):
        assert run(INSTALL_ERASE, **failure)["view"]["stage"] == "write_failed"
        assert "may not start" in text_of("write_failed")

    def test_offers_to_install_again(self, failure):
        assert "install" in actions("write_failed")

    def test_releases_the_port(self, failure):
        assert released(run(INSTALL_ERASE, **failure))

    def test_install_again_asks_for_the_port(self, failure):
        body = INSTALL_ERASE + "t.events.length = 0; await t.flow.install();"
        assert run(body, **failure)["events"][0] == "requestPort"

    def test_stops_warning_before_the_tab_is_closed(self, failure):
        body = INSTALL_ERASE + "t.warned = t.unloadWarns();"
        assert run(body, **failure)["warned"] is False


class TestWhileWriting:
    def test_warns_before_the_tab_is_closed(self):
        assert run(INSTALL)["warned"] is True

    def test_does_not_warn_when_idle(self):
        assert run(INIT + "t.warned = t.unloadWarns();")["warned"] is False

    def test_does_not_warn_when_done(self):
        assert run(INSTALL + "t.warned = t.unloadWarns();")["warned"] is False


class TestWithoutWebSerial:
    def test_says_to_open_it_in_chrome_or_edge_on_a_desktop(self):
        assert run(INIT, noSerial=True)["view"]["stage"] == "unsupported"
        assert "Chrome or Edge on a desktop" in text_of("unsupported")

    def test_offers_no_detect_board(self):
        assert "detect" not in actions("unsupported")


class TestWithoutFirmware:
    def test_a_site_without_targets_json_says_so(self):
        assert run(INIT, targets=None)["view"]["stage"] == "empty"
        assert "No firmware on this copy of the site" in text_of("empty")

    def test_offers_no_detect_board(self):
        assert "detect" not in actions("empty")

    def test_a_blocked_esptool_says_so(self):
        assert run(INIT, noEsptool=True)["view"]["stage"] == "unavailable"

    def test_a_blocked_md5_library_says_so_before_any_erase(self):
        result = run(INIT, noMd5=True)
        assert result["view"]["stage"] == "unavailable"
        assert "erase" not in result["events"]


class TestStartOver:
    def test_from_detected_releases_the_port(self):
        assert released(run(DETECT + "await t.flow.restart();"))

    def test_from_detected_resets_the_board(self):
        assert_reset_then_release(run(DETECT + "await t.flow.restart();")["events"])

    def test_returns_to_idle(self):
        assert run(DETECT + "await t.flow.restart();")["view"]["stage"] == "idle"

    def test_clears_the_erase_choice(self):
        view = run(DETECT + "t.flow.setErase(true); await t.flow.restart();")["view"]
        assert view["erase"] is False

    def test_a_second_board_is_not_erased_from_the_first_boards_tick(self):
        events = run(
            DETECT + "t.flow.setErase(true); await t.flow.restart();" + INSTALL
        )["events"]
        assert "erase" not in events


class TestLiveBoard:
    def test_lights_the_port_when_idle(self):
        assert run(INIT)["view"]["portRing"] == "ring on"

    def test_lights_the_buttons_after_a_failed_connection(self):
        assert run(DETECT, failConnect=True)["view"]["buttonRing"] == "ring on"

    def test_shows_the_revision_on_the_chip(self):
        view = run(DETECT)["view"]
        assert view["revLabel"] == "v1.3"
        assert view["chipClass"] == "chip found"

    def test_lights_the_flash_when_done(self):
        assert run(INSTALL)["view"]["flashClass"] == "flash-part good"


class TestPreRelease:
    """Review focus 1: the latest release predates targets, so only p4-flash.zip is published."""

    @pytest.fixture
    def published(self, tmp_path):
        zips = tmp_path / "zips"
        zips.mkdir()
        with zipfile.ZipFile(zips / "p4-flash.zip", "w") as archive:
            archive.writestr("flash_args", "--flash-mode dio\n0x2000 bootloader.bin\n")
            archive.writestr("bootloader.bin", b"\xff")
        out = tmp_path / "firmware"
        subprocess.run(
            [
                sys.executable,
                str(REPO / "tools" / "flash_manifest.py"),
                str(zips),
                str(out),
                "v0.5.0",
            ],
            check=True,
        )
        return json.loads((out / "targets.json").read_text())

    def test_a_v3_chip_is_told_there_is_no_firmware_yet(self, published):
        result = run(DETECT, targets=published, revision=302)
        assert result["view"]["stage"] == "no_firmware"
        assert result["view"]["rev"] == "v3.2"
        assert result["view"]["release"] == "Release v0.5.0"

    def test_a_v1_chip_gets_the_default_target(self, published):
        view = run(DETECT, targets=published, revision=103)["view"]
        assert view["stage"] == "detected"
        assert view["chipId"] == V1


class TestTemplate:
    def test_every_stage_has_a_panel(self):
        for stage in logic("F.STAGES"):
            panel(stage)

    def test_every_panel_is_a_known_stage(self):
        stages = set(logic("F.STAGES"))
        for match in re.finditer(r'data-stage="([^"]*)"', template()):
            assert set(match.group(1).split()) <= stages

    def test_every_action_is_a_flow_method(self):
        methods = logic("Object.keys(H.makeFlow({}).flow).filter(k => k !== 'state')")
        used = set(re.findall(r'data-action="(\w+)"', template()))
        assert used and used <= set(methods)

    def test_every_binding_is_in_the_view(self):
        keys = run(INIT)["view"].keys()
        used = set(
            re.findall(r'data-(?:text|if|class|href|list|check)="(\w+)"', template())
        )
        assert used and used <= set(keys)

    def test_loads_the_pinned_libraries(self):
        html = template()
        assert "https://unpkg.com/esptool-js@0.7.0/bundle.js" in html
        assert "https://unpkg.com/spark-md5@3.0.2/spark-md5.min.js" in html

    def test_the_guide_uses_the_template(self):
        assert "template: flash.html" in (REPO / "docs" / "flash.md").read_text()
