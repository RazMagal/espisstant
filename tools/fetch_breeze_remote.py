#!/usr/bin/env python3
"""Fetch the IR remote set (IRSet) for a Switcher Breeze remote ID.

The Breeze reports which remote it was configured with (shown in the
Espisstant web UI, e.g. "ELEC7001"). This script pulls that remote's IR
command set from the aioswitcher project's database and stores it where the
build flashes it onto the device's storage partition.

Usage:
    python tools/fetch_breeze_remote.py ELEC7001
    idf.py build flash
"""

import json
import sys
import urllib.request
from pathlib import Path

DB_URL = (
    "https://raw.githubusercontent.com/TomerFi/aioswitcher/dev/"
    "src/aioswitcher/resources/irset_db.json"
)
OUT = Path(__file__).resolve().parent.parent / "spiffs_data" / "breeze_remote.json"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    remote_id = sys.argv[1].strip()

    print(f"downloading IRSet database ...")
    with urllib.request.urlopen(DB_URL) as resp:
        db = json.load(resp)

    if remote_id not in db:
        similar = [k for k in db if k.startswith(remote_id[:4])]
        print(f"remote '{remote_id}' not found in the database.")
        if similar:
            print(f"similar ids: {', '.join(sorted(similar)[:20])}")
        return 1

    irset = db[remote_id]
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(irset, separators=(",", ":")))
    waves = len(irset.get("IRWaveList", []))
    print(f"wrote {OUT} ({waves} IR codes, remote {remote_id})")
    print("now run: idf.py build flash   (reflashes the storage partition)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
