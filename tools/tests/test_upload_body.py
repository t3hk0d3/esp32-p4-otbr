"""p4/pytest/upload_body.py (BDD: OTA latency load request). Host-only; guards against a body without Content-Length."""

import importlib.util
from pathlib import Path

import pytest

MODULE = Path(__file__).resolve().parents[2] / "p4" / "pytest" / "upload_body.py"


@pytest.fixture(scope="module")
def upload_body():
    spec = importlib.util.spec_from_file_location("upload_body", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TestUploadRequest:
    """upload_request"""

    def test_content_length_header_equals_the_body_length(self, upload_body):
        # On any non-empty payload: sets Content-Length to the body length.
        body, headers = upload_body.upload_request(b"x" * 1234)
        assert headers["Content-Length"] == str(body.total) == "1234"

    def test_prepared_request_has_content_length_and_no_chunked_encoding(
        self, upload_body
    ):
        # On a requests POST with this body: the wire request carries Content-Length, not Transfer-Encoding.
        import requests

        body, headers = upload_body.upload_request(b"y" * 5000)
        prepared = requests.Request(
            "POST", "http://board/device/ota", data=body, headers=headers
        ).prepare()
        assert prepared.headers["Content-Length"] == "5000"
        assert "Transfer-Encoding" not in prepared.headers

    def test_reading_the_whole_body_counts_every_byte(self, upload_body):
        # On reading in chunks to the end: read_bytes equals the payload size and fully_read is true.
        body, _ = upload_body.upload_request(b"z" * 10000)
        while body.read(4096):
            pass
        assert body.read_bytes == 10000
        assert body.fully_read

    def test_partial_read_is_not_fully_read(self, upload_body):
        # On reading part of the body: fully_read is false.
        body, _ = upload_body.upload_request(b"z" * 10000)
        body.read(4096)
        assert not body.fully_read
