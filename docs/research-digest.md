# DIGEST RISET — 22 Okt 2026

Ringkasan semua yang dipelajari lewat **reverse engineering langsung** ke binary
vendor `RtWlanU` (Ghidra 12.1.4 via MCP REA) + riset web.
Rujukan detail: [`re-8188e-efuse.md`](re-8188e-efuse.md) (EFUSE/HAL),
[`init-sequence-8188e.md`](init-sequence-8188e.md) (42 langkah),
[`audit-hardening.md`](audit-hardening.md) (40 temuan kernel).

---

## A. Yang diverifikasi lewat RE (tinggi keyakinan)

### A1. Fungsi yang terpetakan

| Alamat | Simbol | Status |
|---|---|---|
| `0x119c0b` | `EFUSE_OneByteRead` | ✅ decompile penuh |
| `0xb1ea5` | `InitOffload_ReadEfuse_8188EU` | ✅ decompile penuh |
| `0xb1bc2` | `ParsingEfuseContent_8188EU` | ✅ decompile penuh |
| `0xb2f7a` | `GetInitialTxBufferBoundary88E` | ✅ decompile penuh |
| `0xb23a9` | `_InitPowerOn88EU` | ✅ (tabel flow 11 entri) |
| `0xaeebc` | `InitCrystalFrequency_8188E` | ✅ **STUB (fungsi kosong)** |
| `0x145d18` | `PHY_RFConfig8188E` | wrapper (18 baris) |
| `0x107481` | `PHY_MACConfig` | 221 baris → 2 sub-fungsi |
| `0x107aa8` | `PHY_BBConfig` | 252 baris → dispatch per chip |
| `0x996ae` | `InitHalDm` | → `dm_ODMStructureInit` (325 baris) |
| `0x99db8` | `FW_InitializeHALVars` | wrapper (20 baris) |

### A2. Register EFUSE 8188E (dipakai driver kita)

| Reg | Fungsi |
|---|---|
| `0x030` | DATA |
| `0x031` | alamat bawah |
| `0x032` | alamat atas (2 bit) |
| `0x033` | status — **bit7 = "siap"** (bukan busy!) |

Polarity: vendor *clear* bit7 = mulai baca, lalu menunggu bit7 **menyala**.

### A3. Register offload 8188E

`0x209` mode · `0x0F0` flag · `0x088` trigger · `0x106` akses EFUSE (`0x69`) ·
`0x140` window · `0x143` status · `0x144` header · `0x148` data.

Mode: `0xaa` (default) atau `0xf4` → base window = `mode << 4`.

### A4. Layout raw stream offload (bukan offset Byte)

```
raw[0..1]          = header stream (dari 0x144 >> 16 / >> 24)
raw[2..5]          = LE(read4(0x148)) blok pertama
raw[k*8+6 .. +9]   = LE(read4(0x144))  ← header blok k
raw[k*8+10.. +13]  = LE(read4(0x148))  ← data blok k
panjang stream     = hdr & 0xffff
```

### A5. Validasi sukses

Image ROM hasil parse harus mulai dengan byte **`0x29`** (`')'`).

### A6. Offset penting di image ROM 8188E

`184` channel plan · `185` xtal · `193` regulatory · **`215..220` MAC**.

### A7. 4 bug implementasi kita yang ditemukan & diperbaiki

| # | Bug | Akibat |
|---|---|---|
| 1 | Trigger 8051 diulang per blok | raw[] kosong → MAC `ff:…` |
| 2 | Layout blok tertukar (0x144 vs 0x148) | parser desinkron → 0xff |
| 3 | Panjang stream parser dipaksa 256 | batas parse salah |
| 4 | Polaritas bit7 register 0x033 terbalik | baca data sebelum siap |

Semua sudah diperbaiki + build + test lulus.

---

## B. Ukur ulang sisa pekerjaan (berubah besar!)

| Milestone | Perkiraan lama | Perkiraan baru | Sumber |
|---|---|---|---|
| M1c — MAC/xtal dari EFUSE | "berat" | **sudah diperbaiki, tinggal 1 reboot** | RE A4–A5 |
| M1d — HAL init (PHY/RF/MAC/DM) | "ribuan baris" | **~450 baris C** (4 fungsi) | RE A1 (ukuran pseudocode) |
| M2 — bulk TX/RX + scan | sedang | kode sudah ada, tinggal validasi | — |
| M3/M4 — net80211 + IO80211 | ~2.000 baris | tetap besar | `docs/m3-m4-integration.md` |

**Pencetus perubahan ini:** `InitCrystalFrequency_8188E` ternyata **stub** →
urutan 8188E yang benar = power-on → DMA → firmware → EFUSE (sudah diterapkan).

---

## C. Temuan dari riset web

_(bagian ini diisi oleh agen riset web paralel; lihat `docs/references.md`)

---

## D. Rujukan eksternal yang dipakai

| Sumber | Peran |
|---|---|
| `upstream/openbsd/sys/dev/usb/if_urtwn.c` | driver asli (ISC) — acuan firmware loader, LLT, scan/join |
| `upstream/itlwm/` | net80211 + AirportItlwm (GPL-2.0) — acuan M3/M4 |
| `vendor/MacKernelSDK/` | header kernel macOS |
| `/Library/Extensions/RtWlanU.kext` | binary vendor — sumber kebenaran semua perilaku |
| `docs/vendor-reverse.md` | metodologi RE |

---

## E. Aturan emas proyek

1. **RE dulu, baru tebak.** Setiap perilaku chip yang tidak jelas → decompile vendor.
   Contoh: 4 bug EFUSE ditemukan hanya lewat RE, bukan lewat review.
2. **RE itu murah.** Tidak perlu reboot, tidak ada risiko kernel panic.
   Semua bug yang ditemukan lewat RE = 0 panic.
3. **Tabel > logika.** Hampir semua init vendor(table-driven) → array
   `static const` di C (pola `rtwn_flow.c`).
4. **Test offline dulu.** Semua parser/protokol punya test tanpa hardware.
5. **Linter untuk kelas bug berulang** (`lint-locks.py`).

