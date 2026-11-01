"""tools/make_bundle.py (BDD: make_bundle.py)."""

import hashlib
import importlib.util
import struct
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent.parent / "make_bundle.py"
HEADER = struct.Struct("<IHHI32s")


@pytest.fixture(scope="module")
def make_bundle():
    spec = importlib.util.spec_from_file_location("make_bundle", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def built(make_bundle, tmp_path):
    """Bundle built from a web image with trailing 0xFF; returns (bundle bytes, trimmed web, app image, out path)."""
    web_content = bytes(range(1, 200)) * 3
    app = b"\xe9" + bytes(range(256)) * 4
    web = tmp_path / "web_0.bin"
    web.write_bytes(web_content + b"\xff" * 5000)
    app_path = tmp_path / "p4.bin"
    app_path.write_bytes(app)
    out = tmp_path / "otbr.bundle"
    assert (
        make_bundle.main(["--web", str(web), "--app", str(app_path), "--out", str(out)])
        == 0
    )
    return out.read_bytes(), web_content, app, out


class TestMakeBundle:
    """make_bundle.py: On web image with trailing 0xFF"""

    def test_web_len_excludes_the_trailing_ff_bytes(self, built):
        bundle, web_content, app, _ = built
        magic, version, reserved, web_len, _ = HEADER.unpack_from(bundle)
        assert (magic, version, reserved) == (0x4242544F, 1, 0)
        assert bundle[:4] == b"OTBB"
        assert web_len == len(web_content)
        assert bundle[HEADER.size : HEADER.size + web_len] == web_content
        assert bundle[HEADER.size + web_len :] == app

    def test_web_sha256_is_over_the_trimmed_bytes(self, built):
        bundle, web_content, _, _ = built
        sha = HEADER.unpack_from(bundle)[4]
        assert sha == hashlib.sha256(web_content).digest()

    def test_inspect_prints_the_header_fields_of_a_bundle_it_produced(
        self, make_bundle, built, capsys
    ):
        _bundle, web_content, app, out = built
        assert make_bundle.main(["--inspect", str(out)]) == 0
        text = capsys.readouterr().out
        assert "magic: 0x4242544f" in text
        assert "format_version: 1" in text
        assert f"web_len: {len(web_content)}" in text
        assert f"web_sha256: {hashlib.sha256(web_content).hexdigest()}" in text
        assert f"app_len: {len(app)}" in text

    def test_inspect_rejects_a_file_that_is_not_a_bundle(self, make_bundle, tmp_path):
        junk = tmp_path / "junk.bin"
        junk.write_bytes(b"\x00" * 100)
        assert make_bundle.main(["--inspect", str(junk)]) == 1

    def test_all_ff_web_image_is_refused(self, make_bundle, tmp_path):
        web = tmp_path / "web.bin"
        web.write_bytes(b"\xff" * 64)
        app = tmp_path / "p4.bin"
        app.write_bytes(b"\xe9" * 64)
        assert (
            make_bundle.main(
                ["--web", str(web), "--app", str(app), "--out", str(tmp_path / "o")]
            )
            == 1
        )
