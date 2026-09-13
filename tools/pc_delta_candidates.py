#!/usr/bin/env python3
import argparse
import pathlib

PAGE_OFFSET = 0xFFFFFFC000000000
RAM_START = 0x80000000
RAM_END = 0x280000000
SECTION = 1 << 30
MASK64 = (1 << 64) - 1


def parse_mm(path):
    text = pathlib.Path(path).read_text(encoding="ascii").strip()
    for token in text.splitlines():
        if token.startswith("mm=0x") or token.startswith("mm="):
            return int(token.split("=", 1)[1], 16)
    return int(text, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("answer", nargs="?", default="ks_mm_answer.txt")
    ap.add_argument("--index", type=int, help="write this candidate index")
    ap.add_argument("--out", help="write index to this file")
    args = ap.parse_args()

    mm = parse_mm(args.answer)
    if not PAGE_OFFSET <= mm < 0xFFFFFFFFFFFFFFFF:
        raise SystemExit(f"mm is outside the linear map: {mm:#018x}")

    diff = mm - PAGE_OFFSET
    first = RAM_START - diff
    end = RAM_END - diff
    memstart = (first + SECTION - 1) & ~(SECTION - 1)
    candidates = []
    while memstart < end:
        phys = memstart + diff
        if RAM_START <= phys < RAM_END:
            delta = (memstart - RAM_START) & MASK64
            candidates.append((memstart, delta, phys))
        memstart += SECTION

    print(f"mm={mm:#018x} diff={diff:#x} candidates={len(candidates)}")
    for index, (base, delta, phys) in enumerate(candidates):
        selected = " <selected>" if args.index == index else ""
        print(f"KS_DELTA candidate[{index}] memstart={base:#018x} "
              f"delta={delta:#018x} mm_phys={phys:#018x}{selected}")
    if args.index is not None:
        if not 0 <= args.index < len(candidates):
            ap.error(f"index {args.index} outside 0..{len(candidates) - 1}")
        if args.out:
            pathlib.Path(args.out).write_text(f"{args.index}\n", encoding="ascii")


if __name__ == "__main__":
    main()
