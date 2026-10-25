# RE LANGSUNG 8188E — initializer EFUSE (22 Okt 2026)

Semua di bawah ini **didecompilasi langsung** dari binær vendor
`RtWlanU.kext` (sha256 `7e8e7a4d…`) dengan Ghidra 12.1.4 lewat MCP REA.
Sumber: `docs/vendor-reverse.md` (metodologi) + dokumen ini (hasil).

> **K disseminate**: contoh di bawah adalah transkripsi pseudocode Ghidra
> (bukan source asli), dipakai sebagai acuan implementasi.

---

## 1. Peta fungsi yang relevan

| Alamat | Simbol | Peran |
|---|---|---|
| `0x119c0b` | `EFUSE_OneByteRead(ADAPTER*, uint offset, uint8_t* out, char flag)` | baca 1 byte EFUSE via register |
| `0x119e0e` | `efuse_GetEfuseDefinition(...)` | definisi/polygon mask |
| `0x11a8ac` | `EFUSE_GetEfuseDefinition(...)` | wrapper |
| `0xb1ea5` | `InitOffload_ReadEfuse_8188EU(ADAPTER*)` | **baca EFUSE lewat mesin internal 8051** |
| `0xb1bc2` | `ParsingEfuseContent_8188EU(...)` | parse raw stream → image ROM |
| `0xaeebc` | `InitCrystalFrequency_8188E(ADAPTER*)` | **STUB (fungsi kosong!)** |
| `0xb23a9` | `_InitPowerOn88EU(ADAPTER*)` | power-on (tabel flow) |
| `0xb2f7a` | `GetInitialTxBufferBoundary88E(ADAPTER*)` | pilih mode offload 0xaa/0xf4 |
| `0xbba8` | `PlatformEFIORead1Byte(void*, uint)` | **=`PlatformIORead1Byte`** (nama historis) |
| `0x71887` | `HAL_8051Reset(ADAPTER*)` | reset 8051 |

Fungsi HAL yang **belum** kita port (langkah vendor #11–#42):

| Alamat | Simbol |
|---|---|
| `0x996ae` | `InitHalDm` |
| `0x99db8` | `FW_InitializeHALVars` |
| `0x107481` | `PHY_MACConfig` |
| `0x107aa8` | `PHY_BBConfig` |
| `0x145d18` | `PHY_RFConfig8188E` |
| `0xb2f3d` | `_InitQueueReservedPage_8188E` |
| `0xb2faf` | `_InitTxBufferBoundary_8188E` |

---

## 2. `EFUSE_OneByteRead` — jalur direct (register 0x031/32/33/30)

```c
undefined1 EFUSE_OneByteRead(ADAPTER *p, uint offset, uint8_t *out, char flag)
{
    ADAPTER *sc = p->sc;

    /* (A) Coba jalur HAL tingkat tinggi dulu (flag == 0). */
    if (flag == 0) {
        if (HAL_MAC_EFUSE_Read1Byte(sc, offset & 0xffff, out, 1, 1) == 0)
            return 1;
    }

    cardType = *(short *)(p + 0x3a0);
    if (cardType == 0x32)
        return Hal_EfuseOneByteRead_8710B(p, offset & 0xffff, out);

    /* (B) Prasyarat: matikan bit "EFUSE mode" di MAC register 0x34. */
    if (sc[0x20c63] != 0 &&
        ((cardType - 0x0b) < 3 ||
         ((cardType - 0x11) < 3 && DAT_00777b78[sc]) ||
         ((cardType - 0x0e) < 3 && (DAT_00777b78[sc] & 0xfe) == 8)))
        PHY_SetMacReg(p, 0x34, 0x800, 0);

    /* (C) Protokol 4 register. */
    PlatformIOWrite1Byte(p, 0x31, offset & 0xff);          /* addr low  */
    b = PlatformEFIORead1Byte(p, 0x32);
    PlatformIOWrite1Byte(p, 0x32, (b & 0xfc) | ((offset >> 8) & 3)); /* addr high */
    b = PlatformEFIORead1Byte(p, 0x33);
    PlatformIOWrite1Byte(p, 0x33, b & 0x7f);                /* clear bit7 = MULAI */
    c = PlatformEFIORead1Byte(p, 0x33);
    if (-1 < c) {                    /* c = char signed → bit7 CLEAR */
        do {
            PlatformForceStallExecution(1000);   /* 1 ms */
            c = PlatformEFIORead1Byte(p, 0x33);
        } while (-1 < c);            /* ← tunggu bit7 MENYALA (siap) */
        if (counter > 99) { *out = 0xff; *(uint32_t*)(p + 0x80) = 2; return 0; }
    }
    *out = PlatformEFIORead1Byte(p, 0x30);   /* DATA */
    return 1;
}
```

### ⚠️ Temuan kritis: polaritas bit 7

`c` bertipe **`char` (signed)** → `-1 < c` berarti **bit7 CLEAR**.
Jadi: **clear bit7 = mulai baca, tunggu bit7 MENYALA = selesai.**

Implementasi kita sebelumnya menunggu bit7 **mati** →-polaritas terbalik → data stale/0xff.
**Sudah diperbaiki 22 Okt 2026** (`src/urtwn/rtwn_efuse_8188e.c`).

### Jalur cache (flag != 0)

Vendor **cache** seluruh image EFUSE di dalam adapter lalu membacanya dari memory:

```c
if (*(uint16_t *)(sc + 0x3ce6) <= offset) return 0;      /* di luar jangkauan */
if (*(byte *)(sc + 0x8cc) == 0)
    p = sc + 0x8d4 + offset;                              /* bank 0 */
else
    p = sc + 0x24e0 + sc[0x8cc] * 0x400 + offset;         /* bank ke-n */
*out = *p;
```

→ `sc[0x8cc]` = indeks **bank** (0 atau n). Inilah asal "broadcast 2× (2 bank)" di
dokumen lama: image yang sama ditulis di beberapa bank.

**Implikasi:** untuk implementasi kita, cukup baca **satu** bank (bank 0).

---

## 3. `InitOffload_ReadEfuse_8188EU` — jalur OFFLOAD (UTAMA untuk 8188E)

```c
int InitOffload_ReadEfuse_8188EU(ADAPTER *p)
{
    ADAPTER *sc = p->sc;
    uint8_t mode = 0xaa;

    /* (1) Pilih mode: 0xf4 hanya untuk cardType 0x0e..0x10 + flag USB. */
    if ((cardType - 0x0e) < 3 && (mode = 0xf4, (sc[5] & 0x80) == 0))
        mode = 0xaa;

    /* (2) ★ POWER-ON DI DALAM FUNGSI INI */
    rc = _InitPowerOn88EU(p);
    sc[DAT_00782876] = (rc == 0);

    /* (3) Mode + base window */
    PlatformIOWrite1Byte(p, 0x209, mode);
    base = mode * 0x10;                 /* 0xaa0 atau 0xf40 */

    for (attempt = 0; attempt < 3; attempt++) {
        /* (4) TRIGGER 8051 — SEKALI per attempt */
        b = PlatformEFIORead1Byte(p, 0x0f0);
        PlatformIOWrite1Byte(p, 0x0f0, b | 0x80);
        HAL_8051Reset(p);
        b = PlatformEFIORead1Byte(p, 0x088);
        PlatformIOWrite1Byte(p, 0x088, b | 0x02);
        for (i = 0; i < 1000; i++) {                    /* stall 50 µs */
            b = PlatformEFIORead1Byte(p, 0x088);
            if ((b & 0x02) == 0) break;                /* bit1 = "busy" */
        }
        if (i > 999) { /* timeout: clear 0x0f0, reset 8051, gagal */ }

        /* (5) Akses EFUSE + window pertama */
        PlatformIOWrite1Byte(p, 0x106, 0x69);           /* EFUSE access ON */
        PlatformIOWrite2Byte(p, 0x140, base);          /* window */
        PlatformIOWrite1Byte(p, 0x143, 0);
        for (i = 0; i < 50; i++) {                      /* stall 50 µs */
            b = PlatformEFIORead1Byte(p, 0x143);
            if (b & 0x01) break;                        /* bit0 = siap */
        }
        hdr0 = PlatformEFIORead4Byte(p, 0x144);
        d1   = PlatformEFIORead4Byte(p, 0x148);

        if ((hdr0 & 0xffff) < 0x101) {
            /* (6) Susun raw stream */
            raw[0] = hdr0 >> 16;      /* header byte 0 */
            raw[1] = hdr0 >> 24;      /* header byte 1 */
            raw[2..5] = LE(d1);       /* read4(0x148) blok pertama */

            nblocks = ((hdr0 & 0xffff) - 8) >> 3;
            extra    = (hdr0 & 7) != 0;
            total    = nblocks + extra;

            for (k = 0; k < total; k++) {
                window = base + 1 + k;
                PlatformIOWrite2Byte(p, 0x140, window);
                PlatformIOWrite1Byte(p, 0x143, 0);
                for (i = 0; i < 50; i++) {              /* stall 50 µs */
                    b = PlatformEFIORead1Byte(p, 0x143);
                    if (b & 0x01) break;
                }
                hdr2 = PlatformEFIORead4Byte(p, 0x144);   /* ← HEADER blok */
                data = PlatformEFIORead4Byte(p, 0x148);   /* ← DATA blok   */
                raw[k*8 + 6  .. k*8 + 9]  = LE(hdr2);
                raw[k*8 + 10 .. k*8 + 13] = LE(data);
            }

            /* (7) Ekor 0xff: dari (rawlen-1) s/d rawlen + (hdr&7) + 5 */
            for (j = 1; j < (hdr0 & 7) + 8; j++)
                raw[j + (hdr0 & 0xffff) - 2] = 0xff;

            /* (8) Parse: panjang stream = (hdr0 & 0xffff) */
            EFUSE_GetEfuseDefinition(p, 0, 4, &def, 0);
            ParsingEfuseContent_8188EU(p, raw, out_rom, def, (short)hdr0, 0);
            memcpy(sc + 0x4c6, out_rom, 0x200);
        }
        /* (9) Bersihkan: clear 0x0f0, reset 8051 */
    }
}
```

### 🔴 Tiga bug implementasi kita yang ditemukan di sini

| # | Vendor | Kita (sebelum 22 Okt) | Akibat |
|---|---|---|---|
| **B1** | Trigger 8051 **sekali per attempt** | Trigger **per blok** | Tiap blok dapat hasil kosong → raw[] 0xff → MAC `ff:ff:ff:ff:ff:ff` |
| **B2** | `raw[k*8+6] = LE(0x144)`, `raw[k*8+10] = LE(0x148)` | `raw[k*8+6] = LE(0x148)` (tertukar) | Stream salah → parser desinkron → 0xff |
| **B3** | Panjang stream untuk parser = `hdr & 0xffff` | Selalu 256 | Batas parse salah |

Sudah diperbaiki: `src/urtwn/rtwn_seq_efuse.c` → `rtwn_seq_read_efuse_offload()`
ditulis ulang mengikuti decompilation di atas (mode 0xaa **dan** 0xf4 dicoba).

### Register offload 8188E (ringkasan)

| Register | Fungsi |
|---|---|
| `0x209` | mode offload (`0xaa` / `0xf4`) |
| `0x0F0` | flag enable offload (set bit7 = mulai; clear bit7 = selesai) |
| `0x088` | trigger (set bit2 = mulai; tunggu bit1 = 0) |
| `0x106` | EFUSE access ON (`0x69`) |
| `0x140` | alamat window (16 bit) = `mode << 4` + blok |
| `0x143` | status (bit0 = data siap) |
| `0x144` | header blok (32 bit) — bit0-15 = panjang stream |
| `0x148` | data blok (32 bit) |

---

## 4. `ParsingEfuseContent_8188EU` — parser (VERIFIED ✓)

Signature:

```c
void ParsingEfuseContent_8188EU(ADAPTER *p,
        uint8_t *raw,        /* stream mentah          */
        uint8_t *out,        /* image ROM 512 byte     */
        short    out_len,    /* dari EFUSE_GetEfuseDefinition */
        uint16_t raw_len,    /* = hdr & 0xffff         */
        uint8_t  out_offset);
```

Inti loop — **DIREVISI 22 Oktober 2026** setelah diuji dengan dump EFUSE
NYATA dari chip (lihat §4b). Versi lama (transkripsi Ghidra) **salah** pada
dua hal dan menghasilkan `rom[0] != 0x29`.

```
cur = raw[0];
if (cur == 0xff) return;                /* stream kosong */
ri = 1;                                  /* ri sudah menunjuk DATA */
while (cur != 0xff && ri < raw_len) {
    if ((cur & 0x1f) == 0x0f) {         /* header ekstensi 2 byte */
        mask = raw[ri] & 0x0f;
        if (mask != 0x0f) {
            off = (raw[ri] >> 1 & 0x78) | (cur >> 5);
            ri++;                        /* lewati byte ke-2 header */
        } else {                         /* bentuk header lain */
            ri += 2; cur = raw[ri];
            continue;                    /* TIDAK memproses blok */
        }
    } else {
        off  = cur >> 4;
        mask = cur & 0x0f;
        /* TIDAK ada ri++ di sini — ri sudah menunjuk data */
    }
    for (i = 0; i < 4; i++) {
        if ((mask >> i) & 1)             /* bit SET = word ABSENT */
            continue;                    /* 0 byte — TIDAK memajukan ri */
        block[off * 4 + i] = LE16(raw[ri], raw[ri+1]);   /* 2 byte */
        ri += 2;
    }
    cur = raw[ri];
    if (cur != 0xff) ri++;
}
/* 64 blok × 4 short → 512 byte out, little endian */
```

### Perbedaan kunci vs versi lama (dua bug)

| # | Lama (salah) | Baru (benar) | Bukti |
|---|---|---|---|
| **P1** | `ri++` juga di cabang header normal → data mulai di `raw[2]`, `raw[1]` terlewat | tidak ada `ri++`; data mulai `raw[1]` | `raw[0]=0x0e, raw[1]=0x29, raw[2]=0x81` → ID `0x8129` |
| **P2** | bit SET = **1 byte** data (dan `ri++`) | bit SET = **ABSENT** (0 byte) | `4f 30` (mask 0) = 4 word; `2f 3e` (mask 0x0e) = 1 word |

### 4b. Bukti dari hardware (dump nyata, 22 Okt 2026)

256 byte pertama EFUSE dibaca langsung dari chip
(`rtwn_probe --efuse-all`, jalur direct 0x031/32/33/30):

```
0e 29 81 6f 33 ff 00 00 00 cf 40 81 ae 96 2d 03 93 96 11 ef 48 fc 8c 00
11 9b ff 01 00 6c 0b 00 00 00 10 00 0c 00 00 00 00 00 00 20 2d 2d 2d 2d
2d 2d 2d 2d 3c 2d 2d 2d f1 ef 20 a1 20 1a 00 00 00 00 00 0f 30 00 01 00
10 00 00 00 00 2f 3e 00 03 4f 30 57 23 0c 01 43 66 00 98 6f 30 03 8e 9e
bc 24 09 03 52 8f 30 65 61 6c 74 65 6b 0d 03 af 30 38 30 32 2e 31 31 6e
20 cf 30 4e 49 43 0c 03 30 30 45 ef 30 30 34 43 30 30 30 31 00 20 2f 2e
2e 2e 2e 2e 33 32 3c 32 31 31 f1 ef 2c a1 2f 15 00 ff ff ...
```

Hasil parse (empat field independen, semuanya cocok):

| Offset | Nilai | Arti |
|---|---|---|
| 0 | `29 81` | EEPROM ID `0x8129` |
| 208 | `57 23 0c 01` | VID `0x2357`, PID `0x010c` |
| **215** | **`98 03 8e 9e bc 24`** | **MAC address** |
| 223 | `"Realtek"` | vendor string (cocok `struct r88e_rom`) |
| 232 | `"802.11n "` | product string |

Blok yang ditulis dua kali (patch di akhir stream) → **nilai terakhir menang**
(contoh: blok 23 `a1 20 1a` lalu `a1 2f 15` → dipakai `a1 2f 15`).

Golden test: `tools/test_efuse_real.c` (memakai dump di atas).
Test differential lama `tools/test_efuse_diff.c` **dihentikan** dari suite:
referensinya adalah transkripsi pseudocode yang salah dengan cara yang sama,
jadi test itu lulus secara sirkular.

### 4c. Reset 8051 yang benar (prasyarat offload)

`HAL_8051PauseIOWrapper` / `HAL_8051EnableIOWrapper` vendor memilih register
berdasarkan `cardType` (`adapter + 0x3a0`):

| cardType | pause | enable |
|---|---|---|
| `> 0x34` | tidak melakukan apa-apa | — |
| `0x0e` (8188E) | `0x1c &= ~0x02`, `0x1d &= ~0x01`, `0x03 &= ~0x04` | `0x1d \|= 0x01`, `0x03 \|= 0x04` |
| `0x0f` / `0x10` | `0x1c &= ~0x02`, `0x1d &= ~0x08`, `0x03 &= ~0x04` | `0x1d \|= 0x08`, `0x03 \|= 0x04` |
| `0x32` (chip lain) | `0x93 &= ~0x01` (lalu return) | `0x93 \|= 0x01` |

Implementasi lama memakai cabang `0x32` untuk 8188E → 8051 tidak pernah
di-reset dengan benar → trigger offload tidak mengisi buffer → EFUSE 0xff.
Sekarang kedua gaya (A/B) dicoba bergantian dalam 8 kombinasi
`mode 0xaa/0xf4 × gaya A/B`.

**Offset penting di image ROM 8188E (dipakai driver kita):**

| Offset | Isi |
|---|---|
| `184` | channel plan |
| `185` | xtal (crystal frequency) |
| `193` | regulatory domain (`& 0x07`) |
| `215..220` | **MAC address (6 byte)** |

---

## 5. `InitCrystalFrequency_8188E` adalah STUB

```
void InitCrystalFrequency_8188E(ADAPTER *p) { return; }
```

Artinya untuk 8188E **langkah vendor #2 tidak melakukan apa-apa** — xtal dibaca
dari cache EFUSE (offset 185) di langkah lain. Konsekuensi: urutan yang benar
untuk 8188E adalah **power-on → DMA → firmware → EFUSE (offload)**.

---

## 6. Fungsi HAL yang belum diport (urutan 42 langkah)

Langkah vendor yang masih perlu diimplementasikan:

| # | Vendor | Fungsi |
|---|---|---|
| 11 | `_InitRDGSetting_8188E` | — |
| 12 | `InitHalDm` | `0x996ae` |
| 14 | `FW_InitializeHALVars` | `0x99db8` |
| 15 | `PHY_MACConfig` | `0x107481` |
| 16 | `PHY_BBConfig` | `0x107aa8` |
| 17 | `PHY_RFConfig8188E` | `0x145d18` |
| 18 | `_InitQueueReservedPage_8188E` | `0xb2f3d` |
| 19 | `_InitTxBufferBoundary_8188E` | `0xb2faf` |

### Ukuran kerja HAL (diukur via RE, 22 Okt 2026)

Struktur panggilan (dari `procedure_callees` + pseudocode):

```
PHY_MACConfig        (0x107481, 221 baris)  -> phy_ConfigMACWith (50)
                                               odm_config_mac_with_header_file (73)
PHY_BBConfig        (0x107aa8, 252 baris)  -> phy_BB8188E_Config_ (153)
                                               + 10 varian chip lain
PHY_RFConfig8188E   (0x145d18,  18 baris)  -> wrapper tipis
InitHalDm            (0x996ae,  11 baris)  -> dm_ODMStructureInit (325)
FW_InitializeHALVars (0x99db8,  20 baris)  -> wrapper tipis
```

| Fungsi | Alamat | Ukuran pseudocode | Peran |
|---|---|---|---|
| `phy_BB8188E_Config_` | `0x14593c` | ~153 baris | konfigurasi **baseband** 8188E |
| `dm_ODMStructureInit` | `0x981cf` | ~325 baris | inisialisasi struktur **DM/ODM** |
| `odm_config_mac_with_header_file` | `0x1efdcc` | ~73 baris | tabel konfigurasi MAC |
| `phy_ConfigMACWith` | `0x104802` | ~50 baris | menerapkan tabel MAC |

**Total ~600 baris pseudocode -> +-450 baris C portabel.**

Jadi M1d (HAL init) jauh lebih kecil dari perkiraan awal (bukan ribuan
baris) -- bandingkan dengan 8.000+ baris kode yang sudah ada.

Sebagian besar fungsi ini **table-driven** (tabel register), sama seperti
`rtwn_flow.c` yang sudah kita punya untuk power-on. Pola yang sama bisa
dipakai lagi: dump tabel dari vendor -> array `static const` di C.


---

## 7. Cara mereproduksi RE ini

```bash
# 1. Binary vendor (read-only)
sudo mv /Library/Extensions/RtWlanU.kext /Library/Extensions/RtWlanU.kext.off
#   (atau pakai yang di /Library/Extensions.disabled-m1/)

# 2. Buka lewat MCP REA (Ghidra 12.1.4)
open_binary({path: "<path>/RtWlanU", provider_id: "ghidra"})

# 3. Cari & decompile
search_procedures({pattern: "InitOffload_ReadEfuse"})
procedure_pseudo_code({procedure: "0xb1ea5"})

# 4. Tutup (sesi Ghidra bersifat ephemeral)
close_binary()
```

Semua yang di atas bisa dilakukan **tanpa menyentuh hardware** — inilah
kenapa RE adalah jalur teraman untuk iterasi.

---

## 8. ⭐ Rekonsiliasi dengan FreeBSD/OpenBSD (riset web, 22 Okt 2026)

Agen riset web menemukan **FreeBSD** punya pembaca EFUSE untuk 8188E
(`sys/dev/rtwn/if_rtwn_efuse.h`) dan `struct r88e_rom`
(`sys/dev/rtwn/rtl8188e/r88e_rom_image.h`).

### 8.1 Layout `struct r88e_rom` (konfirmasi independen)

```
0x00 reserved1[16]
0x10 cck_tx_pwr[6]
0x16 ht40_tx_pwr[5]
0x1B tx_pwr_diff
0x1C reserved2[156]
0xB8 channel_plan        (184)  ← cocok dengan offset kita
0xB9 crystalcap          (185)  ← xtal kita ✓
0xBA thermal_meter       (186)
0xBB reserved3[6]
0xC1 rf_board_opt        (193)  ← "regulatory" kita ✓
0xC2 rf_feature_opt
0xC3 rf_bt_opt
0xC4 version
0xC5 customer_id
0xC6 reserved4[3]
0xC9 rf_ant_opt
0xCA reserved5[6]
--- union diff_d0 (0xD0) ---
USB variant : vid(2)@0xD0  pid(2)@0xD2  usb_opt(1)@0xD4
              reserved6[2]   macaddr[6]@0xD7   ← MAC = 215..220 ✓✓
PCI variant : macaddr[6]@0xD0  vid@0xD6  pid@0xD8
```

`R88E_EFUSE_MAX_LEN 256` (fisik) / `R88E_EFUSE_MAP_LEN 512` (logis) —
cocok dengan Linux `EFUSE_REAL_CONTENT_LEN_88E` / `EFUSE_MAP_LEN_88E`.

**Konsekuensi:** image EFUSE chip kita harus berisi **VID 0x2357 di 0xD0** dan
**PID 0x010c di 0xD2**. Ini sekarang dipakai sebagai **validasi kedua**
(lebih kuat daripada cek MAC) di `rtwn_efuse_8188e.c` &
`rtwn_seq_efuse.c`.

### 8.2 ⚠️ Koreksi penting: akses 0x30 (32-bit) dan 0x031/32/33 (per-byte) itu SAMA

Dulu kita menyimpulkan *"OpenBSD menulis address ke 0x030 — pada 8188E itu
register DATA, jadi pembacaan OpenBSD tidak pernah sukses"*.

**Temuan baru: itu belum tentu benar.** Register 0x030 pada 8188E, dibaca/
ditulis sebagai **32 bit**, constituted oleh byte-byte yang sama:

```
0x030 byte0 = DATA      ← dibaca per-byte oleh gaya 8188E
0x031 byte1 = addr low  ← addr = bits 8..17 dari word 32-bit
0x032 byte2 = addr high (2 bit)
0x033 byte3 = status (bit7 = ready; pada word 32-bit = bit31 = VALID)
```

Jadi **pembaca 92C-style (OpenBSD/FreeBSD) dan pembaca 8188E-style (vendor
Windows) addressed register yang sama** — berbeda cara akses, bukan register
berbeda. FreeBSDonomous mengirim dukungan 8188E USB selama bertahun-tahun
dengan pembaca 92C-style → **pembaca 92C-style memang berfungsi untuk chip
ini**.

**Implikasi:** jalur `rtwn_seq_read_efuse()` (gaya 92C) di proyek kita **valid**,
dan harus dipertahankan sebagai fallback.

### 8.3 🐛 Bug reader yang ditemukan: berhenti di byte 0xFF pertama

Pembaca EFUSE yang benar bersifat **header-driven**, bukan "berhenti saat 0xff":

```
loop:
  hdr = byte(addr)
  if (hdr == 0xff) break          ← 0xff HANYA = akhir map kalau yang
  off = hdr >> 4;                    dibaca adalah HEADER
  msk = hdr & 0x0f
  words = popcount(~msk & 0x0f)
  baca words × 2 byte data
  addr += 1 + words*2
```

Byte `0xff` **bisa muncul di tengah blok data** (sel EFUSE yang belum
diprogram). Reader lama kita berhenti di sana → map terpotong → MAC hilang.

**Perbaikan:** baca seluruh 256 byte fisiknya; parser
(`efuse_parse_8188e`) yang menghitung sendiri jumlah byte data dari mask,
jadi ia tahu kapan map berakhir.

### 8.4 Catatan lain

* FreeBSD memakai `xtal = crystal_cap & 0x3f` lalu menulis ke
  `R92C_AFE_XTAL_CTRL` (`addr | addr << 6`) — default `0x20`.
* OpenBSD commit **`rtwn: add rtwn_efuse_preread` (23 Agu 2026)** — sangat
  baru; menambah preread EFUSE per-chip untuk 88E.
* `R92C_EFUSE_ACCESS = 0x0cf` / `ON = 0x69` — identik dengan Linux
  `REG_EFUSE_ACCESS = 0x00CF` / `EFUSE_ACCESS_ON = 0x69` ✓

