#!/usr/bin/env python3
"""Generate the deterministic PCM16 WAV fixtures for the VoiceTyper C++ migration.

Usage
-----
    python3 tools/generate-wav-fixtures/generate_wav_fixtures.py
    python3 tools/generate-wav-fixtures/generate_wav_fixtures.py <output-directory>

Default output directory: tests/fixtures/migration/wav (relative to the repo root).

Exit codes
----------
    0  every fixture was written and passed the built-in container self-check
    1  a write or a self-check failed (message on stderr)
    2  bad usage (argparse)

Why this script exists and what it deliberately does NOT do
-----------------------------------------------------------
It writes *input* fixtures only. It does not run, reimplement or approximate the
C# conversion path (WavBuilder resampling/downmix, SilenceTrimmer trimming,
WavPcmReader parsing). Every "expected" number recorded in
tests/fixtures/migration/manifest.json is derived by reading the C# sources, not
by executing .NET, and no C++ parity claim is made anywhere.

Determinism
-----------
Byte-for-byte identical output on every host, which rules out ``math.sin``: the C
library behind it has no specified accuracy, so the last bits of a tone -- and
therefore of the PCM16 file -- would differ between libm builds, and a
truncating cast to int16 can turn a last-bit difference into a full 1 LSB
difference. Every sample here is produced by a sine recurrence evaluated in
``decimal.Decimal`` with 60 significant digits using only +, -, * and division by
small exact integers. libmpdec is correctly rounded and platform independent, so
the recurrence and the truncation of its result are identical everywhere. The
pi literal is asserted against the platform double at start-up.

Rounding
--------
Tone samples are truncated toward zero, which is what the C# cast ``(short)x``
does for an in-range double. Two scalings are used, and both are recorded in the
manifest:

* 16383.5  -- ``short.MaxValue / 2``. This is literally the formula of
  WavBuilderTests.GenerateSinePcm16, ``(short)(Math.Sin(2*PI*f*i/sr) * short.MaxValue * 0.5)``,
  so the two rate/width fixtures are bit-comparable with a C# run up to the libm
  question above.
* amplitude * 32768 -- for the SilenceTrimmer fixtures. The C# tests feed float
  arrays to SilenceTrimmer directly and never go through PCM16; writing PCM16 and
  reading it back as ``int16 / 32768`` (WavPcmReader's own conversion) reproduces
  the intended float amplitude to within 1 LSB = 3.05e-5, which is four orders
  of magnitude below the 0.005 amplitude those tests are about.

License safety
--------------
No audio, voice, model, or user data is involved: every sample is either a
440 Hz tone or an exact zero. The fixtures are license-safe by construction.

Verification
------------
This script only writes. ``tools/verify-migration-fixtures.py`` is the
independent checker: it re-reads every file, re-walks the RIFF chunk list and
compares the recorded byte count and SHA-256 against the manifest.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import math
import struct
import sys
from decimal import Decimal, getcontext
from pathlib import Path
from typing import Callable, Iterable, Sequence

getcontext().prec = 60

# pi to 60+ significant digits. Cross-checked against the platform double below.
PI = Decimal(
    "3.141592653589793238462643383279502884197169399375105820974944592307816406286"
)

# WavBuilderTests.GenerateSinePcm16 scaling: (short)(Math.Sin(...) * short.MaxValue * 0.5)
CSHARP_SINE_SCALE = Decimal(32767) / 2  # 16383.5

# WavPcmReader divides int16 by 32768, so this is the inverse of a full-scale round trip.
PCM_FULL_SCALE = 32768

TARGET_SAMPLE_RATE = 16000  # WavBuilder.TargetSampleRate
TONE_HZ = 440


def _check_pi_literal() -> None:
    """Guard the pi literal: it must agree with the platform double to 1e-15."""
    if abs(PI - Decimal(math.pi)) >= Decimal("1e-15"):
        raise RuntimeError("PI literal does not match the platform pi double")


def _taylor_sin(x: Decimal) -> Decimal:
    """sin(x) = x - x^3/3! + x^5/5! - ... by Taylor series in Decimal.

    Only used for a handful of small angles (the largest is 2*pi*440/16000).
    """
    term = x
    total = x
    x2 = x * x
    for n in range(1, 200):
        term = -term * x2 / Decimal((2 * n) * (2 * n + 1))
        total += term
        if term == 0:
            break
    return total


def _taylor_cos(x: Decimal) -> Decimal:
    """cos(x) = 1 - x^2/2! + x^4/4! - ... by Taylor series in Decimal."""
    term = Decimal(1)
    total = Decimal(1)
    x2 = x * x
    for n in range(200):
        if term == 0:
            break
        # t(n+1) = -t(n) * x^2 / ((2n+1)(2n+2)), from t(n) = (-1)^n x^(2n) / (2n)!
        term = -term * x2 / Decimal((2 * n + 1) * (2 * n + 2))
        total += term
    return total


def _check_trig() -> None:
    """Guard both series before any sample is produced.

    Three independent invariants, because a wrong recurrence denominator is
    silent otherwise: the small-angle limits, the quadratic term of the cosine
    series, and the Pythagorean identity at the largest angle this script uses.
    """
    largest = Decimal(2) * PI * Decimal(440) / Decimal(16000)  # 2*pi*440/16000
    s = _taylor_sin(largest)
    c = _taylor_cos(largest)
    if abs(s * s + c * c - 1) > Decimal("1e-50"):
        raise RuntimeError("Taylor sin/cos do not satisfy sin^2+cos^2 == 1")

    small = Decimal("1e-12")
    sin_expected = small - small * small * small / 6
    if abs(_taylor_sin(small) - sin_expected) > Decimal("1e-55"):
        raise RuntimeError("Taylor sin does not match x - x^3/6")
    cos_expected = Decimal(1) - small * small / 2 + small ** 4 / 24
    if abs(_taylor_cos(small) - cos_expected) > Decimal("1e-55"):
        raise RuntimeError("Taylor cos does not match 1 - x^2/2 + x^4/24")


def _truncate(value: Decimal) -> int:
    """Toward-zero conversion, matching the C# ``(short)`` cast for in-range doubles."""
    return int(value)


def tone(sample_count: int, rate: int, scale: Decimal, frequency: int = TONE_HZ) -> list[int]:
    """First ``sample_count`` int16 samples of ``scale * sin(2*pi*frequency*i/rate)``.

    The phase restarts at i = 0 for every call, exactly like the ``Tone``/``Speech``
    helpers of SilenceTrimmerTests, so repeating a segment in ``Concat`` repeats
    the same samples.
    """
    step = 2 * PI * Decimal(frequency) / Decimal(rate)
    step_sin = _taylor_sin(step)
    step_cos = _taylor_cos(step)
    s = Decimal(0)
    c = Decimal(1)
    out: list[int] = []
    append = out.append
    for _ in range(sample_count):
        value = _truncate(s * scale)
        if value > 32767 or value < -32768:
            raise RuntimeError(f"tone sample {value} does not fit in int16 (scale={scale})")
        append(value)
        s, c = s * step_cos + c * step_sin, c * step_cos - s * step_sin
    return out


def silence(sample_count: int) -> list[int]:
    return [0] * sample_count


def frames(seconds: float, rate: int = TARGET_SAMPLE_RATE) -> int:
    """Frame count for a duration, using the C# ``(int)(rate * seconds)`` cast."""
    return int(rate * seconds)


def pack_pcm16(samples: Sequence[int]) -> bytes:
    """Little-endian int16 payload, portable across endianness."""
    buffer = array.array("h", samples)
    if buffer.itemsize != 2:
        raise RuntimeError(f"unexpected array('h') itemsize {buffer.itemsize}")
    if sys.byteorder != "little":
        buffer.byteswap()
    return buffer.tobytes()


def fmt_chunk(
    channels: int,
    rate: int,
    bits: int = 16,
    audio_format: int = 1,
) -> bytes:
    block_align = channels * bits // 8
    byte_rate = rate * block_align
    return b"fmt " + struct.pack(
        "<IHHIIHH", 16, audio_format, channels, rate, byte_rate, block_align, bits
    )


def riff(chunks: bytes) -> bytes:
    """RIFF/WAVE container whose size field covers exactly the file that follows."""
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def data_chunk(samples: Sequence[int]) -> bytes:
    payload = pack_pcm16(samples)
    return b"data" + struct.pack("<I", len(payload)) + payload


def canonical_wav(samples: Sequence[int], channels: int, rate: int) -> bytes:
    """Canonical 44-byte-header PCM16 WAV (RIFF/fmt /data, no padding needed)."""
    return riff(fmt_chunk(channels, rate) + data_chunk(samples))


# --------------------------------------------------------------------------- #
# Fixtures
# --------------------------------------------------------------------------- #


def wav_sine_44k_mono() -> bytes:
    """WavBuilderTests.ConvertTo16KHzMonoWav_FromMono44K_Returns16KHzMono16Bit input.

    440 Hz, 1.0 s, 44100 Hz, mono, PCM16, amplitude 0.5 * short.MaxValue.
    Expected (per that test, NOT executed here): 16000 Hz mono PCM16 WAV,
    TotalTime in [0.95, 1.05] s. The tone has no silent frames, so
    SilenceTrimmer keeps every sample.
    """
    samples = tone(frames(1.0, 44100), 44100, CSHARP_SINE_SCALE)
    return canonical_wav(samples, channels=1, rate=44100)


def wav_sine_48k_stereo() -> bytes:
    """WavBuilderTests.ConvertTo16KHzMonoWav_FromStereo48K_ReturnsMono16KHz input.

    440 Hz, 0.5 s, 48000 Hz, stereo, PCM16; the same value in both channels so
    that NAudio's StereoToMonoSampleProvider average is the identity.
    Expected (per that test): mono 16 kHz PCM16, TotalTime in [0.45, 0.55] s.
    """
    mono = tone(frames(0.5, 48000), 48000, CSHARP_SINE_SCALE)
    interleaved: list[int] = []
    for value in mono:
        interleaved.append(value)
        interleaved.append(value)
    return canonical_wav(interleaved, channels=2, rate=48000)


def wav_silence_speech_silence() -> bytes:
    """SilenceTrimmerTests.Trim_RemovesLeadingAndTrailingSilence input.

    16 kHz mono: 0.5 s of exact zeros + 1.0 s of 440 Hz amplitude 0.5 + 0.5 s of
    exact zeros. Same shape as the 1.0/0.5/1.0 s case of
    WavBuilderTests.ConvertTo16KHzMonoWav_TrimsLeadingAndTrailingSilence.
    Expected: trimming keeps 24000 samples = exactly 1.5 s (1.0 s of speech plus
    the 0.25 s margin on each side); the C# test asserts [1.4, 1.6] s.
    """
    speech = tone(frames(1.0), TARGET_SAMPLE_RATE, Decimal("0.5") * PCM_FULL_SCALE)
    samples = silence(frames(0.5)) + speech + silence(frames(0.5))
    return canonical_wav(samples, channels=1, rate=TARGET_SAMPLE_RATE)


def wav_silence_quietspeech_silence() -> bytes:
    """SilenceTrimmerTests.Trim_QuietSpeech_BelowFixedThreshold_IsNotCut input.

    16 kHz mono: 0.3 s of zeros + 1.0 s of 440 Hz amplitude 0.005 + 0.3 s of zeros.
    The amplitude is below the retired fixed 0.01 threshold but above the noise
    floor, so the adaptive threshold must keep it.
    Expected: trimming keeps 24000 samples = 1.5 s; the C# test asserts [1.0, 1.8] s.
    """
    quiet = tone(frames(1.0), TARGET_SAMPLE_RATE, Decimal("0.005") * PCM_FULL_SCALE)
    samples = silence(frames(0.3)) + quiet + silence(frames(0.3))
    return canonical_wav(samples, channels=1, rate=TARGET_SAMPLE_RATE)


def wav_long_internal_pause() -> bytes:
    """SilenceTrimmerTests.Trim_LongInternalPause_KeepsBothPhrasesAndCompressesGap input.

    16 kHz mono: 5 s zeros + 1 s tone + 5 s zeros + 1 s tone + 5 s zeros (17.0 s).
    Expected: the kept window is samples [76000, 196000); the 5 s internal pause
    is compressed to the 0.3 s gap, leaving 44800 samples = exactly 2.8 s; the C#
    test asserts [2.4, 3.2] s. Trim_LongInternalPause_DoesNotContainTheLongPause
    additionally requires the result to be under 4.0 s.
    """
    speech = tone(frames(1.0), TARGET_SAMPLE_RATE, Decimal("0.5") * PCM_FULL_SCALE)
    samples = (
        silence(frames(5.0)) + speech + silence(frames(5.0)) + speech + silence(frames(5.0))
    )
    return canonical_wav(samples, channels=1, rate=TARGET_SAMPLE_RATE)


def wav_noncanonical_chunks() -> bytes:
    """WavPcmReaderTests.To16KHzMonoFloats_ParsesNonCanonicalChunks, byte for byte.

    A LIST/INFO chunk sits between WAVE and fmt , and the 8 bytes the C# test
    leaves after the data chunk are reproduced as trailing zeros. Single sample
    -16384, so a correct parser yields exactly one float, -0.5.
    """
    chunks = (
        b"LIST" + struct.pack("<I", 4) + b"INFO"
        + fmt_chunk(channels=1, rate=TARGET_SAMPLE_RATE)
        + data_chunk([-16384])
        + b"\x00" * 8
    )
    return riff(chunks)


def wav_noncanonical_oddpad_chunk() -> bytes:
    """Odd-sized chunk padding, the case the LIST fixture above does not cover.

    WavPcmReader steps the chunk cursor by ``8 + size + (size & 1)``; a 3-byte
    chunk therefore consumes a pad byte. This file has a 3-byte JUNK chunk before
    fmt  and another 3-byte JUNK chunk after data, so both the leading and the
    trailing pad are exercised. The data samples are the full-scale quadruple of
    WavPcmReaderTests.To16KHzMonoFloats_ConvertsPlusMinusFullScale, whose expected
    floats are 0.99996948, -1.0, 0.0, 0.5.
    """
    junk_head = b"JUNK" + struct.pack("<I", 3) + b"abc" + b"\x00"
    junk_tail = b"JUNK" + struct.pack("<I", 3) + b"\x01\x02\x03" + b"\x00"
    chunks = (
        junk_head
        + fmt_chunk(channels=1, rate=TARGET_SAMPLE_RATE)
        + data_chunk([32767, -32768, 0, 16384])
        + junk_tail
    )
    return riff(chunks)


def wav_reject_stereo() -> bytes:
    """WavPcmReaderTests.To16KHzMonoFloats_RejectsStereo, byte for byte.

    A structurally valid 16 kHz PCM16 WAV with channels = 2. A parser matching
    the C# reference must reject it with NotSupportedException ("Требуется моно
    WAV."), so the fixture is only ever valid as a negative case.
    """
    chunks = fmt_chunk(channels=2, rate=TARGET_SAMPLE_RATE) + data_chunk([0, 0])
    return riff(chunks)


def wav_reject_nonpcm_format28() -> bytes:
    """WavPcmReaderTests.To16KHzMonoFloats_RejectsNonPcmFormat, byte for byte.

    audioFormat = 28 (MPEG ADPCM) with a 16-bit sample width, which is
    self-contradictory on purpose. A parser matching the C# reference must reject
    it with NotSupportedException ("Требуется PCM-формат WAV (audioFormat=1)").
    """
    chunks = (
        fmt_chunk(channels=1, rate=TARGET_SAMPLE_RATE, audio_format=28) + data_chunk([0, 0])
    )
    return riff(chunks)


# id -> builder. The order here is the order printed and written.
FIXTURES: dict[str, Callable[[], bytes]] = {
    "wav-sine-44k-mono": wav_sine_44k_mono,
    "wav-sine-48k-stereo": wav_sine_48k_stereo,
    "wav-silence-speech-silence": wav_silence_speech_silence,
    "wav-silence-quietspeech-silence": wav_silence_quietspeech_silence,
    "wav-long-internal-pause": wav_long_internal_pause,
    "wav-noncanonical-chunks": wav_noncanonical_chunks,
    "wav-noncanonical-oddpad-chunk": wav_noncanonical_oddpad_chunk,
    "wav-reject-stereo": wav_reject_stereo,
    "wav-reject-nonpcm-format28": wav_reject_nonpcm_format28,
}


# --------------------------------------------------------------------------- #
# Self-check
# --------------------------------------------------------------------------- #


def walk_chunks(blob: bytes) -> list[tuple[bytes, int, int]]:
    """Minimal RIFF chunk walker used only by the writer's self-check."""
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE container")
    riff_size = struct.unpack_from("<I", blob, 4)[0]
    if riff_size + 8 != len(blob):
        raise ValueError(f"RIFF size {riff_size + 8} != file size {len(blob)}")
    found: list[tuple[bytes, int, int]] = []
    offset = 12
    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        size = struct.unpack_from("<I", blob, offset + 4)[0]
        end = offset + 8 + size
        if end > len(blob):
            raise ValueError(f"chunk {chunk_id!r} at {offset} exceeds the file")
        found.append((chunk_id, size, offset + 8))
        offset = end + (end & 1)
    return found


def self_check(fixture_id: str, blob: bytes) -> None:
    """Container-level assertions only. No behavior is claimed by this check."""
    chunks = walk_chunks(blob)
    ids = [chunk_id for chunk_id, _, _ in chunks]
    if b"fmt " not in ids:
        raise ValueError(f"{fixture_id}: no fmt chunk")
    if b"data" not in ids:
        raise ValueError(f"{fixture_id}: no data chunk")
    for chunk_id, size, start in chunks:
        if chunk_id == b"fmt ":
            _audio_format, channels, rate, byte_rate, align, bits = struct.unpack_from(
                "<HHIIHH", blob, start
            )
            if size < 16:
                raise ValueError(f"{fixture_id}: fmt chunk is {size} bytes")
            if align != channels * bits // 8:
                raise ValueError(f"{fixture_id}: block align {align} is inconsistent")
            if byte_rate != rate * align:
                raise ValueError(f"{fixture_id}: byte rate {byte_rate} is inconsistent")
        elif chunk_id == b"data":
            if size % 2 != 0:
                raise ValueError(f"{fixture_id}: odd data size {size} for PCM16")


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    repo_root = Path(__file__).resolve().parents[2]
    parser.add_argument(
        "output_directory",
        nargs="?",
        type=Path,
        default=repo_root / "tests" / "fixtures" / "migration" / "wav",
        help="default: tests/fixtures/migration/wav",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        _check_pi_literal()
        _check_trig()
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    output_directory: Path = args.output_directory
    try:
        output_directory.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"error: cannot create {output_directory}: {exc}", file=sys.stderr)
        return 1

    print(f"# output: {output_directory}")
    print(f"# python: {sys.version.split()[0]}  precision: {getcontext().prec} digits")
    for fixture_id, build in FIXTURES.items():
        try:
            blob = build()
            self_check(fixture_id, blob)
        except (RuntimeError, ValueError) as exc:
            print(f"error: {fixture_id}: {exc}", file=sys.stderr)
            return 1
        path = output_directory / f"{fixture_id}.wav"
        try:
            path.write_bytes(blob)
        except OSError as exc:
            print(f"error: {path}: {exc}", file=sys.stderr)
            return 1
        digest = sha256(blob)
        print(f"{fixture_id}.wav\t{len(blob)}\t{digest}")

    print(f"generated {len(FIXTURES)} fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
