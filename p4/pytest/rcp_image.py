"""Re-stamp the version of an ESP application image (c6_rcp.bin) without a rebuild.

The version lives in `esp_app_desc_t.version` (32 bytes at offset 16 of the descriptor, which starts the first
segment's data). Changing it needs the image checksum and the appended SHA-256 to be recomputed.
"""

import hashlib

HEADER_LEN = 24
SEGMENT_HEADER_LEN = 8
APP_DESC_VERSION_OFFSET = HEADER_LEN + SEGMENT_HEADER_LEN + 16
VERSION_LEN = 32
CHECKSUM_SEED = 0xEF
HASH_APPENDED_OFFSET = 23
MAGIC = 0xE9
ALIGNMENT = 16


def segments(image):
    """(data_offset, length) of each segment."""
    assert image[0] == MAGIC, "not an ESP image"
    found = []
    offset = HEADER_LEN
    for _ in range(image[1]):
        length = int.from_bytes(image[offset + 4 : offset + 8], "little")
        found.append((offset + SEGMENT_HEADER_LEN, length))
        offset += SEGMENT_HEADER_LEN + length
    return found, offset


def version_of(image):
    raw = image[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + VERSION_LEN]
    return raw.split(b"\0", 1)[0].decode()


def retag(image, version):
    """Copy of `image` whose application version is `version` (checksum and SHA-256 recomputed)."""
    assert len(version) < VERSION_LEN
    body = bytearray(image)
    body[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + VERSION_LEN] = (
        version.encode().ljust(VERSION_LEN, b"\0")
    )
    parts, end = segments(body)
    checksum = CHECKSUM_SEED
    for offset, length in parts:
        for byte in body[offset : offset + length]:
            checksum ^= byte
    checksum_at = (
        end + ALIGNMENT
    ) // ALIGNMENT * ALIGNMENT - 1  # last byte of the 16-byte block after the segments
    body[checksum_at] = checksum
    if body[HASH_APPENDED_OFFSET]:
        body[checksum_at + 1 : checksum_at + 1 + 32] = hashlib.sha256(
            body[: checksum_at + 1]
        ).digest()
    return bytes(body)
