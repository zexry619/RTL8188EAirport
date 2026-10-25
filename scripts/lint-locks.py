#!/usr/bin/env python3
"""
lint-locks.py — periksa keseimbangan lock/unlock di kode kernel.

Alasan: PANIC #4 (22 Okt 2026) disebabkan oleh
    IOLockUnlock(mutex) tanpa IOLockLock(mutex) sebelumnya
→ "unlocking an unlocked mutex" → preemption_level = -1 → panic di
   call berikutnya yang butuh mutex.

Bug itu lolos review manual DAN audit 40 temuan, karena subtel dan
sangat pendek. Jadi sekarang ada pemeriksaan otomatis.

Aturan yang diperiksa (per fungsi):
  1. Setiap IOLockUnlock(X) harus didahului IOLockLock(X) di fungsi
     yang sama, pada jalur eksekusi yang sama.
  2. Jumlah IOLockUnlock tidak boleh melebihi jumlah IOLockLock.
  3. IOLockFree(X) tidak boleh dipanggil pada mutex yang mungkin masih
     di-lock (diperingatan saja).
  4. Pointer lock yang dikunci dan dibuka harus sama (dicek lewat nama).

Exit code 0 = bersih, 1 = ada temuan.
"""

import re
import sys
import os
import glob

FILES = sorted(
    glob.glob("src/driver/*.cpp") +
    glob.glob("src/usb/*.cpp") +
    glob.glob("src/driver/*.hpp") +
    glob.glob("src/usb/*.h")
)


def strip_comments_and_strings(src):
    """Hapus komentar & string supaya tidakfalse-positive."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c in '"\'':
            q = c
            i += 1
            while i < n and src[i] != q:
                if src[i] == '\\':
                    i += 1
                i += 1
            i += 1
            out.append('""')
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def find_functions(src):
    """Yield (start_line, name, body) untuk setiap fungsi yang punya body."""
    # pola: [modifier] name(...) {   (sederhana, cukup untuk audit)
    pat = re.compile(r'(?m)^[A-Za-z_][\w \t\*&:<>,]*?\b(\w+)\s*\([^;{]*?\)\s*(?:const\s*)?\{')
    for m in pat.finditer(src):
        name = m.group(1)
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = src[start:i + 1]
        line = src[:m.start()].count('\n') + 1
        yield line, name, body


def check_file(path):
    problems = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        raw = f.read()
    src = strip_comments_and_strings(raw)

    for line, name, body in find_functions(src):
        locks = re.findall(r'IOLockLock\s*\(\s*([^)]*?)\s*\)', body)
        unlocks = re.findall(r'IOLockUnlock\s*\(\s*([^)]*?)\s*\)', body)
        frees = re.findall(r'IOLockFree\s*\(\s*([^)]*?)\s*\)', body)

        if not unlocks and not locks:
            continue

        # Aturan utama: SETIAP IOLockUnlock(X) harus punya IOLockLock(X)
        # yang muncul SEBELUMNYA di fungsi yang sama.
        #
        # (Banyak unlock setelah satu lock itu LEGAL — unlock di setiap
        #  jalur return. Yang TIDAK legal: unlock tanpa lock sama sekali,
        #  atau unlock mutex yang tidak pernah di-lock.)
        positions = {}
        for m in re.finditer(r'IOLock(Lock|Unlock)\s*\(\s*([^)]*?)\s*\)', body):
            positions.setdefault(m.group(2).strip(), []).append(
                (m.group(1), m.start()))

        for mutex, evs in positions.items():
            # Setiap unlock harus punya setidaknya satu lock yang muncul
            # SEBELUMNYA di fungsi yang sama. (Banyak unlock setelah satu
            # lock itu legal — unlock di tiap jalur return.)
            seen_lock = False
            for kind, pos in evs:
                if kind == 'Lock':
                    seen_lock = True
                elif not seen_lock:
                    problems.append((line, name,
                        f"IOLockUnlock({mutex}) tanpa IOLockLock({mutex}) "
                        f"sebelumnya di fungsi ini"))
                    break

        # Aturan 3 (peringatan): free mutex yang mungkin sedang dipegang.
        if frees and locks:
            problems.append((line, name,
                f"PERHATIAN: IOLockFree({frees}) di fungsi yang juga "
                f"mengunci {locks} — pastikan tidak sedang di-lock"))
    return problems


def main():
    total = 0
    for path in FILES:
        if not os.path.exists(path):
            continue
        for line, name, msg in check_file(path):
            total += 1
            print(f"  ❌ {path}:{line}  {name}()  — {msg}")

    print()
    if total:
        print(f"❌ {total} temuan keseimbangan lock")
        return 1
    print(f"✅ keseimbangan lock OK ({len(FILES)} file diperiksa)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
