#!/usr/bin/env python3
"""Small deterministic smoke test for the differential comparator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMPARATOR = ROOT / "tools" / "compare-contract-json.py"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="vt-diff-") as temp:
        directory = Path(temp)
        reference = directory / "reference.json"
        candidate = directory / "candidate.json"
        different = directory / "different.json"
        reference.write_text(
            json.dumps({"recordingMode": "vad", "modelSize": "small", "silenceThresholdMs": 1200}),
            encoding="utf-8",
        )
        candidate.write_text(
            json.dumps({"recordingMode": 2, "modelSize": "SMALL", "silenceThresholdMs": 1200}),
            encoding="utf-8",
        )
        different.write_text(
            json.dumps({"recordingMode": "vad", "modelSize": "medium", "silenceThresholdMs": 1200}),
            encoding="utf-8",
        )

        same = subprocess.run(
            [sys.executable, str(COMPARATOR), "--normalize-enums", str(reference), str(candidate)],
            check=False,
            capture_output=True,
            text=True,
        )
        if same.returncode != 0:
            print(same.stdout, end="")
            print(same.stderr, end="", file=sys.stderr)
            return same.returncode

        mismatch = subprocess.run(
            [sys.executable, str(COMPARATOR), "--normalize-enums", str(reference), str(different)],
            check=False,
            capture_output=True,
            text=True,
        )
        if mismatch.returncode != 1:
            print("differential comparator did not reject a real difference", file=sys.stderr)
            return 1

    print("differential-compare-smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
