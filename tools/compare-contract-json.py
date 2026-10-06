#!/usr/bin/env python3
"""Compare two contract JSON documents with deterministic diagnostics.

The .NET reference and the C++ implementation may serialize enums as their
legacy numeric form or canonical camelCase form. With --normalize-enums both
are reduced to a canonical string for the named enum keys before comparison.
All other differences remain hard failures: a contract test must not hide a
lost or invented field.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

ENUM_FIELDS = {
    "recordingMode": "pushToTalk|toggle|vad",
    "language": "auto|ru|en",
    "modelSize": "tiny|base|small|medium|large",
    "transcriptionEngine": "whisper|parakeet",
    "parakeetModelSize": "q4K|q5K|q6K|q8_0",
    "theme": "system|light|dark",
    "appLanguage": "ru|en",
}


def enum_value(field: str, value: Any) -> Any:
    if field not in ENUM_FIELDS or value is None:
        return value
    names = ENUM_FIELDS[field].split("|")
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and int(value) == value:
        index = int(value)
        return names[index] if 0 <= index < len(names) else f"__out_of_range__:{index}"
    if isinstance(value, str):
        for index, name in enumerate(names):
            if value.casefold() == name.casefold():
                return name
    return value


def normalize(value: Any, *, enums: bool) -> Any:
    if isinstance(value, dict):
        return {
            key: (enum_value(key, item) if enums else item)
            if enums and key in ENUM_FIELDS
            else normalize(item, enums=enums)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [normalize(item, enums=enums) for item in value]
    return value


def compare(reference: Any, candidate: Any, path: str, differences: list[str]) -> None:
    if type(reference) is not type(candidate) and not (
        isinstance(reference, (int, float))
        and isinstance(candidate, (int, float))
        and not isinstance(reference, bool)
        and not isinstance(candidate, bool)
    ):
        differences.append(f"{path}: type {type(reference).__name__} != {type(candidate).__name__}")
        return
    if isinstance(reference, dict):
        for key in sorted(set(reference) | set(candidate)):
            if key not in reference:
                differences.append(f"{path}.{key}: candidate-only field")
            elif key not in candidate:
                differences.append(f"{path}.{key}: missing from candidate")
            else:
                compare(reference[key], candidate[key], f"{path}.{key}", differences)
        return
    if isinstance(reference, list):
        if len(reference) != len(candidate):
            differences.append(f"{path}: length {len(reference)} != {len(candidate)}")
            return
        for index, (left, right) in enumerate(zip(reference, candidate)):
            compare(left, right, f"{path}[{index}]", differences)
        return
    if isinstance(reference, float) or isinstance(candidate, float):
        if not math.isclose(float(reference), float(candidate), rel_tol=0.0, abs_tol=1e-9):
            differences.append(f"{path}: {reference!r} != {candidate!r}")
    elif reference != candidate:
        differences.append(f"{path}: {reference!r} != {candidate!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--normalize-enums", action="store_true")
    args = parser.parse_args()

    reference = json.loads(args.reference.read_text(encoding="utf-8-sig"))
    candidate = json.loads(args.candidate.read_text(encoding="utf-8-sig"))
    if args.normalize_enums:
        reference = normalize(reference, enums=True)
        candidate = normalize(candidate, enums=True)

    differences: list[str] = []
    compare(reference, candidate, "$", differences)
    if differences:
        for difference in differences:
            print(f"FAIL {difference}", file=sys.stderr)
        return 1
    print("differential-compare: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
