"""Check that every SIG_* in src/core/signatures.h matches exactly once in its client DLL.

Usage: python tools/verify_sigs.py <client_dir>   (or set CLARITY_CLIENT)
"""
import os
import re
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parent.parent
SIG_RE = re.compile(r'#define\s+(SIG_\w+)\s+"([^"]+)",\s*"([^"]+)"')


def parse_pattern(text):
    return [None if tok == "??" else int(tok, 16) for tok in text.split()]


def find_all(data, pattern):
    first = pattern[0]
    hits = []
    start = 0
    n = len(pattern)
    while True:
        i = data.find(bytes([first]), start) if first is not None else start
        if i < 0 or i + n > len(data):
            return hits
        if all(p is None or data[i + k] == p for k, p in enumerate(pattern)):
            hits.append(i)
        start = i + 1


def main():
    client = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CLARITY_CLIENT")
    if not client:
        sys.exit(__doc__.strip())
    client = Path(client)
    sigs = SIG_RE.findall((ROOT / "src/core/signatures.h").read_text())
    images = {}
    ok = True
    for name, module, pattern in sigs:
        if module not in images:
            pe = pefile.PE(str(client / module))
            images[module] = (pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase)
        image, base = images[module]
        hits = find_all(image, parse_pattern(pattern))
        status = "OK" if len(hits) == 1 else "FAIL"
        ok &= len(hits) == 1
        where = ", ".join(hex(base + h) for h in hits[:5])
        print(f"{status:4} {name:22} {module:18} {len(hits)} match(es) {where}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
