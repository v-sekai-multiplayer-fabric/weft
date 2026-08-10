#!/usr/bin/env python3
# Cheap vs nasty protocol comparison on the real SUMO trace.
#
# nasty = bitpacked C struct: per entity (u32 slot, f32 x, f32 y) = 12 bytes.
#         This is the hot-path format (memcpy/cast decode, ~1 ns/entity in C).
# cheap = CBOR-encoded JSON-LD: self-describing, interoperable, for the debug
#         and interop edge, never the hot path.
#
# Reports raw bytes, zstd level 1, and zstd with the previous frame as the
# dictionary (last-frame delta), plus python encode/decode time as a relative
# signal. The nasty hot-path decode number comes from bench/pps_native.c in C.
import struct
import time

import cbor2
import zstandard as zstd

FRAMES = "scenario/frames.bin"
CTX = {"@vocab": "https://weft.dev/sumo#", "e": "slot", "x": "x", "y": "y"}


def load_frames(path):
    with open(path, "rb") as fh:
        magic, max_slots, nframes = struct.unpack("<III", fh.read(12))
        assert magic == 0x53554D4F
        frames = []
        for _ in range(nframes):
            (count,) = struct.unpack("<I", fh.read(4))
            buf = fh.read(count * 12)
            ents = [struct.unpack_from("<Iff", buf, i * 12) for i in range(count)]
            frames.append(ents)
    return frames, max_slots


def nasty_encode(frame):
    out = bytearray(struct.pack("<I", len(frame)))
    for slot, x, y in frame:
        out += struct.pack("<Iff", slot, x, y)
    return bytes(out)


def nasty_decode(buf):
    (count,) = struct.unpack_from("<I", buf, 0)
    return [struct.unpack_from("<Iff", buf, 4 + i * 12) for i in range(count)]


def cheap_encode(frame, step):
    doc = {
        "@context": CTX,
        "@type": "Frame",
        "step": step,
        "entities": [{"e": s, "x": x, "y": y} for s, x, y in frame],
    }
    return cbor2.dumps(doc)


def cheap_decode(buf):
    doc = cbor2.loads(buf)
    return [(o["e"], o["x"], o["y"]) for o in doc["entities"]]


def zstd_sizes(blobs):
    c1 = zstd.ZstdCompressor(level=1)
    raw = sum(len(b) for b in blobs)
    indep = sum(len(c1.compress(b)) for b in blobs)
    delta = 0
    prev = None
    for b in blobs:
        if prev is None:
            delta += len(c1.compress(b))
        else:
            cd = zstd.ZstdCompressor(level=1, dict_data=zstd.ZstdCompressionDict(prev))
            delta += len(cd.compress(b))
        prev = b
    return raw, indep, delta


def timed(fn, n):
    t0 = time.perf_counter()
    fn()
    return (time.perf_counter() - t0) / n * 1e6  # microseconds per frame


def main():
    frames, max_slots = load_frames(FRAMES)
    n = len(frames)
    ents = sum(len(f) for f in frames)
    print(f"frames={n} max_slots={max_slots} total_entities={ents} "
          f"avg_active={ents / n:.0f}\n")

    nasty = [nasty_encode(f) for f in frames]
    cheap = [cheap_encode(f, i) for i, f in enumerate(frames)]

    n_raw, n_indep, n_delta = zstd_sizes(nasty)
    c_raw, c_indep, c_delta = zstd_sizes(cheap)

    n_enc = timed(lambda: [nasty_encode(f) for f in frames], n)
    n_dec = timed(lambda: [nasty_decode(b) for b in nasty], n)
    c_enc = timed(lambda: [cheap_encode(f, i) for i, f in enumerate(frames)], n)
    c_dec = timed(lambda: [cheap_decode(b) for b in cheap], n)

    def row(name, raw, indep, delta, enc, dec):
        return (f"{name:<8} {raw/1e6:>7.2f} {raw/ents:>8.1f} {indep/1e6:>7.2f} "
                f"{delta/1e6:>7.2f} {raw/delta:>7.1f} {enc:>8.1f} {dec:>8.1f}")

    print(f"{'proto':<8} {'raw MB':>7} {'B/ent':>8} {'z1 MB':>7} {'z-dict':>7} "
          f"{'dict x':>7} {'enc µs':>8} {'dec µs':>8}")
    print(row("nasty", n_raw, n_indep, n_delta, n_enc, n_dec))
    print(row("cheap", c_raw, c_indep, c_delta, c_enc, c_dec))
    print(f"\ncheap/nasty raw = {c_raw / n_raw:.1f}x bytes, "
          f"z-dict = {c_delta / n_delta:.1f}x bytes, "
          f"decode = {c_dec / n_dec:.0f}x slower (python)")


if __name__ == "__main__":
    main()
