#!/usr/bin/env python3
"""forge .brd files byte-by-byte, including deliberately hostile ones.

this is the adversary in this test suite. it does not use brd_format.c,
that is the whole point: it writes the wire format by hand so it can produce
files the real encoder would refuse to emit (unknown field ids, duplicate
fields, trailing payloads, bad checksums), and prove the decoder rejects them.

layout mirrors board/brd_format.h; if that changes, this must change with it.

  usage: forge_brd.py <case> <output-path>
         forge_brd.py list
"""
import struct
import sys
import zlib

MAGIC = b"ARASBRD1"
VERSION = 1

BRAND, MANUFACTURER, MODEL, CODENAME, DEVICE_NAME, AUTHOR = 1, 2, 3, 4, 5, 6

GOOD = [
    (BRAND, b"pocket"),
    (MANUFACTURER, b"Pocket"),
    (MODEL, b"POCKET_P1_A"),
    (CODENAME, b"POCKET_P1"),
    (DEVICE_NAME, b"Pocket P1"),
    (AUTHOR, b"test-suite"),
]


def build(fields, *, magic=MAGIC, version=VERSION, field_count=None,
          payload_length=None, crc=None, trailing=b""):
    payload = b"".join(struct.pack("<BH", fid, len(val)) + val for fid, val in fields)
    if payload_length is None:
        payload_length = len(payload)
    if field_count is None:
        field_count = len(fields)
    header = magic + struct.pack("<HHI", version, field_count, payload_length)
    body = header + payload
    if crc is None:
        crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc) + trailing


CASES = {}


def case(name):
    def wrap(fn):
        CASES[name] = fn
        return fn
    return wrap


@case("valid")
def _valid():
    """a well-formed file, the control for every rejection case below."""
    return build(GOOD)


@case("unknown-field")
def _unknown_field():
    """the hostile case the brief names. field id 7 does not exist in the
    format; a real attacker would pick it hoping the parser copies unknown
    fields into some dictionary that later reaches a property setter. here it
    is dressed up as an soc override, the single most load-bearing thing a
    .brd must never touch."""
    return build(GOOD + [(7, b"Snapdragon_8_Elite")], field_count=7)


@case("smuggled-hardware")
def _smuggled_hardware():
    """same attack, aimed at ro.hardware / phonePropHardware, the value the
    guest's hal selection keys off. field id 9, again nonexistent."""
    return build(GOOD + [(9, b"sun")], field_count=7)


@case("unknown-field-exact-count")
def _unknown_field_exact():
    """the sharpest version of the same attack: still exactly six fields, so a
    parser that only counts them sees nothing wrong, but the codename record
    is swapped for unknown id 8 carrying a board name. this is the case that
    genuinely exercises the unknown-id rejection rather than a count check."""
    return build([f for f in GOOD if f[0] != CODENAME] + [(8, b"sun")])


@case("duplicate-field")
def _duplicate():
    """two brands: a parser that took the last one and a ui that showed the
    first would disagree about what is being installed."""
    return build(GOOD + [(BRAND, b"evil")], field_count=7)


@case("missing-author")
def _missing_author():
    """the author field is required and may never be blank."""
    return build([f for f in GOOD if f[0] != AUTHOR])


@case("empty-author")
def _empty_author():
    return build([(f, b"" if f == AUTHOR else v) for f, v in GOOD])


@case("bad-magic")
def _bad_magic():
    return build(GOOD, magic=b"NOTABRD1")


@case("bad-version")
def _bad_version():
    """a future format this build must refuse rather than misparse."""
    return build(GOOD, version=99)


@case("bad-crc")
def _bad_crc():
    return build(GOOD, crc=0xDEADBEEF)


@case("trailing-bytes")
def _trailing():
    """a valid file with a payload appended after the checksum."""
    return build(GOOD, trailing=b"\x00" * 32)


@case("length-lie")
def _length_lie():
    """header claims a longer payload than the file holds, the classic
    over-read. must be caught before any record is touched."""
    return build(GOOD, payload_length=4000)


@case("shell-metacharacters")
def _shell_meta():
    """`; rm -rf` in a brand. these values reach a json config and a process
    environment, so the charset, not downstream quoting, is what has to stop
    this."""
    return build([(f, b"pocket; rm -rf ~" if f == BRAND else v) for f, v in GOOD])


@case("newline-injection")
def _newline():
    """a newline in the device name. anything line-oriented downstream (a
    kernel command line, a .conf, a log) would see two records."""
    return build([(f, b"Pocket\nro.product.brand=evil" if f == DEVICE_NAME else v) for f, v in GOOD])


@case("embedded-nul")
def _embedded_nul():
    """a value that looks short to a c string reader and long to the parser,
    the truncation trick. `pocket\\0evil` must not validate as `pocket`."""
    return build([(f, b"pocket\x00evil" if f == BRAND else v) for f, v in GOOD])


@case("oversized-value")
def _oversized():
    return build([(f, b"P" * 200 if f == MODEL else v) for f, v in GOOD])


@case("non-ascii")
def _non_ascii():
    return build([(f, "Pöcket".encode("utf-8") if f == MANUFACTURER else v) for f, v in GOOD])


@case("padded-value")
def _padded():
    """leading space: invisible in inspect output, so two files could look
    identical and behave differently."""
    return build([(f, b" Pocket" if f == MANUFACTURER else v) for f, v in GOOD])


@case("truncated")
def _truncated():
    return build(GOOD)[:12]


@case("empty-file")
def _empty():
    return b""


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "list":
        for name in CASES:
            print(name)
        return 0
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    name, path = sys.argv[1], sys.argv[2]
    if name not in CASES:
        print(f"forge_brd.py: unknown case '{name}'", file=sys.stderr)
        return 2
    with open(path, "wb") as handle:
        handle.write(CASES[name]())
    return 0


if __name__ == "__main__":
    sys.exit(main())
