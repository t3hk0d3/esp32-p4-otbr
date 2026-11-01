"""Request body for the OTA latency load: a real Content-Length and a count of the bytes the client handed to the socket.

A generator body would make `requests` send `Transfer-Encoding: chunked` without Content-Length, which the board refuses
at once (`length_required`): the load would never run. `tools/tests/test_upload_body.py` guards this on the host.
"""

import io

LINK_LIMIT_BYTES_PER_S = (
    12_000_000  # 100 Mbit/s Ethernet: an upload cannot finish faster than this
)


class UploadBody(io.BytesIO):
    """BytesIO that counts the bytes read out of it (the bytes `requests` sent)."""

    def __init__(self, data):
        super().__init__(data)
        self.total = len(data)
        self.read_bytes = 0

    def read(self, size=-1):
        chunk = super().read(size)
        self.read_bytes += len(chunk)
        return chunk

    @property
    def fully_read(self):
        return self.read_bytes >= self.total


def upload_request(data):
    """(body, headers) for POST /device/ota: Content-Length equals the body length."""
    body = UploadBody(data)
    return body, {
        "Content-Length": str(body.total),
        "Content-Type": "application/octet-stream",
    }


def min_upload_seconds(size):
    """Fastest possible time for the board to take `size` bytes over the link."""
    return size / LINK_LIMIT_BYTES_PER_S
