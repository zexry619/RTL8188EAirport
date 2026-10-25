#!/usr/bin/env python3
"""Bersihkan entri AuxiliaryKernelExtensions berdasarkan pola bundle-id.

Usage: clean-auxkc.py <pola1> [pola2 ...]
Contoh: clean-auxkc.py rtwlanu
"""
import glob
import os
import plistlib
import shutil
import sys

BASE = "/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC"


def matches(text, patterns):
    t = str(text).lower()
    return any(p.lower() in t for p in patterns)


def main():
    patterns = sys.argv[1:]
    if not patterns:
        print("pola tidak diberikan", file=sys.stderr)
        return 1

    for name in ("com.apple.kcgen.instructions.plist", "com.apple.kcgen.uakl.plist"):
        path = os.path.join(BASE, name)
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            data = plistlib.load(f)
        changed = False

        ktb = data.get("kextsToBuild", {})
        for key in list(ktb):
            if matches(key, patterns):
                del ktb[key]
                changed = True

        for key in ("SignedKernelExtensions", "UnsignedKernelExtensions"):
            value = data.get(key)
            if isinstance(value, dict):
                for team in list(value):
                    kept = [e for e in value[team] if not matches(e, patterns)]
                    if len(kept) != len(value[team]):
                        changed = True
                        if kept:
                            value[team] = kept
                        else:
                            del value[team]
            elif isinstance(value, list):
                kept = [e for e in value if not matches(e, patterns)]
                if len(kept) != len(value):
                    data[key] = kept
                    changed = True

        if changed:
            shutil.copy(path, path + ".bak")
            with open(path, "wb") as f:
                plistlib.dump(data, f)
            print("[clean-auxkc] %s dibersihkan" % name)

    for path in glob.glob(os.path.join(BASE, "StashedExtensions", "*", "*.kext")):
        if matches(os.path.basename(path), patterns):
            shutil.rmtree(path, ignore_errors=True)
            print("[clean-auxkc] staged %s dihapus" % os.path.basename(path))

    return 0


if __name__ == "__main__":
    sys.exit(main())
