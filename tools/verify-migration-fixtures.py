#!/usr/bin/env python3
"""Verify materialized migration fixtures against the manifest.

This check is intentionally standard-library-only and cross-platform. It does
not validate behavior; it proves that the recorded bytes, hashes and container
shape are stable and reviewable. Negative WAV fixtures marked with
``containerExpectation=reject`` are still structurally verified. When a negative
fixture also declares ``expectedAudioFormat`` (for example format 28), that
value is asserted explicitly so the negative contract cannot silently rot.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_wav(path: Path, expected_audio_format: int | None = 1) -> None:
    """Walk the RIFF chunk list of a WAV fixture.

    ``expected_audio_format=None`` verifies only the structural container and
    is used for negative fixtures whose wrong channel/codec is behavior under
    test. A declared expectedAudioFormat is always asserted.
    """
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE container")
    riff_size = struct.unpack_from("<I", data, 4)[0]
    if riff_size + 8 != len(data):
        raise ValueError(f"RIFF size {riff_size + 8} != file size {len(data)}")
    offset = 12
    saw_fmt = False
    saw_data = False
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        start = offset + 8
        end = start + chunk_size
        if end > len(data):
            raise ValueError(f"chunk {chunk_id!r} exceeds file")
        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise ValueError("fmt chunk is too small")
            audio_format, channels, rate, _byte_rate, _align, bits = struct.unpack_from(
                "<HHIIHH", data, start
            )
            if expected_audio_format is not None and audio_format != expected_audio_format:
                raise ValueError(
                    f"audioFormat {audio_format} != expected {expected_audio_format}"
                )
            if channels == 0 or rate == 0 or bits == 0:
                raise ValueError("invalid fmt chunk")
            saw_fmt = True
        elif chunk_id == b"data":
            saw_data = True
        offset = end + (end & 1)
    if not saw_fmt or not saw_data:
        raise ValueError("WAV must contain fmt and data chunks")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parents[1]
        / "tests"
        / "fixtures"
        / "migration"
        / "manifest.json",
    )
    args = parser.parse_args()
    root = args.manifest.resolve().parents[3]
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    materialized = [f for f in manifest.get("fixtures", []) if "materialized" in f]
    failures: list[str] = []

    for fixture in materialized:
        info = fixture["materialized"]
        path = root / info["path"]
        label = fixture["id"]
        if not path.is_file():
            failures.append(f"{label}: missing {path}")
            continue
        size = path.stat().st_size
        if size != info["bytes"]:
            failures.append(f"{label}: bytes {size} != {info['bytes']}")
        actual_hash = sha256(path)
        if actual_hash != info["sha256"]:
            failures.append(f"{label}: sha256 {actual_hash} != {info['sha256']}")
        try:
            if path.suffix.lower() == ".json":
                json.loads(path.read_text(encoding="utf-8-sig"))
            elif path.suffix.lower() == ".wav":
                if "expectedAudioFormat" in info:
                    expected_format = int(info["expectedAudioFormat"])
                elif info.get("containerExpectation") == "reject":
                    expected_format = None
                else:
                    expected_format = 1
                verify_wav(path, expected_format)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            failures.append(f"{label}: {exc}")
        else:
            suffix = " (negative container verified)" if info.get("containerExpectation") == "reject" else ""
            print(f"PASS {label} ({size} bytes){suffix} {actual_hash}")

    if not materialized:
        print("No materialized fixtures declared", file=sys.stderr)
        return 1
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    print(f"fixture-check: OK ({len(materialized)} materialized fixtures)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
