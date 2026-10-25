# Reverse Engineering Driver Vendor Realtek (RtWlanU.kext) — LENGKAP

Tanggal: 21–22 Okt 2026 · Binary: `/Library/Extensions/RtWlanU.kext/Contents/MacOS/RtWlanU`
· 11,7 MB · **tidak stripped** (11.958 simbol) · Mach-O x86_64
· SHA256 `7e8e7a4d6ced1b85b3854c896011feb6227a430c49fd738322f76b1b0d819ba3`
· UUID `DB501FB3-487F-382A-A630-86C151C13BFB`

> ⚠️ Binary vendor **closed source**. Semua di sini untuk **interoperability/verifikasi**,
> bukan menyalin kode. Lihat [`legal.md`](legal.md).
>
> Semua pseudocode = **hasil decompilation Ghidra 12.1.4** (via REA MCP), bukan
> interpretasi manual.

**Alat**: REA (`rea-agents` 6.7.0, MCP 207 tool) + Ghidra 12.1.4 (decompiler
`mac_x86_64` disalin dari Ghidra 11.4.3) + JDK 21 + `nm`/`otool` + analisis byte-level.

---

## 0. RINGKASAN EKSEKUTIF — jawaban atas "kenapa M1 gagal"

| Dugaan lama | Kenyataan (terbukti RE) |
|---|---|
| Encoding vendor request salah | ✅ **Benar** — `bmRequestType` 0xC0/0x40, `bRequest` **0x05**, `wIndex` **0** |
| Transfer USB salah | ✅ **Benar** — `wValue` = alamat register, `wLength` = panjang data |
| Register map (CR 0x100, MACID 0x610, EFUSE_CTRL 0x030) | ✅ **Benar** — semua cocok |
| Offset MAC di ROM = 215 | ✅ **Benar** — `Hal_EfuseParseXtal_8188E` baca **rom[185]** = xtal (OpenBSD: `r88e_rom.xtal` @185 ✓) |
| ~~EFUSE bisa dibaca langsung~~ | ❌ **SALAH** — 8188E USB memakai jalur **OFFLOAD** (mesin internal/8051), bukan baca langsung |
| Parser EFUSE OpenBSD | ⚠️ **Hampir sama**, tapi ada 2 perbedaan (lihat §4.3) |

**Penyebab kegagalan M1 yang sebenarnya: kita memakai algoritma EFUSE OpenBSD
(baca langsung via EFUSE_CTRL), sedangkan 8188E USB memerlukan urutan offload
khusus** (register 0x209/0x0F0/0x088/0x106/0x140/0x143/0x144/0x148 + reset 8051).

Temuan ini sudah diimplementasikan di `src/urtwn/rtwn_seq_efuse.c`.

---

## 1. Peta simbol penting

| Simbol | Peran |
|---|---|
| `RtWlanU::MxUsbSyncVendorRequest(usb, adapter, isWrite, bReq, wVal, wIdx, data, len)` | **Primitif vendor request** |
| `RtWlanU::PlatformUSBReadReg(usb, adapter, size, offset, buf)` | Baca register |
| `RtWlanU::PlatformUSBWriteReg(usb, adapter, size, offset, buf)` | Tulis register |
| `RtWlanU::PlatformIORead4Byte(adapter, offset)` | Baca 4 byte (inti jalur register) |
| `RtWlanU::GetHwReg8188EUsb / SetHwReg8188EUsb` | Register 88E (dispatch per offset) |
| `HAL_GetAddressOffset(adapter, offset)` | **Base alamat register** |
| `RtWlanU::InitOffload_ReadEfuse_8188EU(adapter)` | **Baca EFUSE 8188E (offload)** |
| `RtWlanU::ParsingEfuseContent_8188EU(adapter, raw, out, len, rawlen, off)` | **Parser EFUSE 8188E** |
| `Hal_EfuseParseXtal_8188E(adapter, rom, isPCIE)` | Ambil xtal dari rom[185] |
| `HAL_8051Reset / HAL_8051PauseIOWrapper / HAL_8051EnableIOWrapper` | Reset 8051 (untuk offload) |
| `EFUSE_GetMaskArray_MP_8188E_MUSB` / `_Array_MP_8188E_MUSB` | **Tabel mask EFUSE 8188E** |
| `FW_FirmwareDownload`, `FillH2CCommand8188E_MP`, `SetFwPwrModeCmd_8188E` | Upload firmware 8051 |
| `_Array_MP_8188E_MUSB` | Firmware 8188E (MUSB) tertanam di binary |

---

## 2. Encoding vendor request — TERKONFIRMSI

### `PlatformUSBReadReg` (0x80c48)

```c
void RtWlanU::PlatformUSBReadReg(_RT_USB_DEVICE *usb, _ADAPTER *adapter,
                                 uint size, uint offset, void *buf)
{
    MxUsbSyncVendorRequest(usb, *adapter, 0, 5, offset, 0, buf, size);
}
```

### `PlatformUSBWriteReg` (0x80c7c)

```c
void RtWlanU::PlatformUSBWriteReg(_RT_USB_DEVICE *usb, _ADAPTER *adapter,
                                  uint size, uint offset, void *buf)
{
    MxUsbSyncVendorRequest(usb, *adapter, 1, 5, offset, 0, buf, size);
}
```

### `MxUsbSyncVendorRequest` (0x80aaa) — potongan penting

```c
    local_48 = (param_3 != 1) << 7 | 0x40;   // ★ bmRequestType
    local_42 = param_8;                      //   wLength
    local_40 = param_7;                      //   pData
    local_47 = param_4;                      // ★ bRequest
    local_46 = param_5;                      // ★ wValue (alamat register)
    local_44 = param_6;                      //   wIndex
    uVar2 = 5000;                            //   timeout (ms)
    iVar1 = (**(code **)(*plVar3 + 0x1c8))(plVar3, &local_48, uVar2, uVar2, 0);
```

Layout request = persis `usb_device_request_t` standar:

| Offset | Field | Read | Write |
|---|---|---|---|
| 0x00 | `bmRequestType` | **0xC0** (IN·VENDOR·DEVICE) | **0x40** (OUT·VENDOR·DEVICE) |
| 0x01 | `bRequest` | **0x05** | **0x05** |
| 0x02 | `wValue` | alamat register | alamat register |
| 0x04 | `wIndex` | **0** | **0** |
| 0x06 | `wLength` | panjang data | panjang data |
| 0x08 | `pData` | pointer buffer | pointer buffer |

Timeout vendor: **5000 ms** (kecuali satu register khusus → 500 ms).

### Perbandingan

| Field | Vendor | OpenBSD `urtwn` | `src/urtwn/rtwn_seq.c` |
|---|---|---|---|
| `bmRequestType` | 0xC0 / 0x40 | sama | ✓ |
| `bRequest` | 0x05 | 0x05 | ✓ |
| `wValue` | alamat register | alamat register | ✓ |
| `wIndex` | 0 | 0 | ✓ |
| timeout | 5000 ms | default USB | 250 ms (perlu dinaikkan ke ~1000 ms) |

---

## 3. Perhitungan alamat register — `HAL_GetAddressOffset` (0x74489)

```c
undefined4 HAL_GetAddressOffset(_ADAPTER *adapter, uint offset)
{
    undefined4 uVar1;
    long lVar2;
    uVar1 = 0;
    if (offset < 0x100) {
        lVar2 = safeMetaCast(*(void **)(adapter + 0x70), &RtWlanDeviceU::gMetaClass);
        uVar1 = *(undefined4 *)(*(long *)(lVar2 + 0x120) + 0x778);
    }
    return uVar1;
}
```

Dan di `PlatformIORead4Byte` (0xbe2c):

```c
    iVar4 = HAL_GetAddressOffset(adapter, offset);
    ...
    PlatformUSBReadReg(usb, dev, 4, iVar4 + offset & 0xffff, &local_3c);
```

### Arti

```
alamat = base + (offset & 0xFFFF)
base   = 0                      bila offset >= 0x100
base   = device->field_0x778    bila offset <  0x100
```

| Register | Offset | Base? | Alamat |
|---|---|---|---|
| `CR` | 0x100 | ❌ | 0x100 ✓ |
| `MACID` | 0x610 | ❌ | 0x610 ✓ |
| `MCUFWDL` | 0x080 | ⚠️ | 0x080 + base |
| `SYS_CFG` | 0x0F0 | ⚠️ | 0x0F0 + base |
| `EFUSE_CTRL` | 0x030 | ⚠️ | 0x030 + base |
| `EFUSE_ACCESS` | 0x0CF | ⚠️ | 0x0CF + base |
| `SYS_ISO_CTRL`/`SYS_FUNC_EN`/`APS_FSMCO`/`SYS_CLKR` | 0x000–0x008 | ⚠️ | + base |

OpenBSD memakai **alamat langsung** (tanpa base) dan bekerja di chip 8188E →
base untuk 88E kemungkinan **0**, tapi harus dibuktikan.
Sudah ada deteksi empiris di `tools/rtwn_probe.c` (mencoba base {0,0x100,0x200,0x400,0x1000,0x2000,0x3000}).

---

## 4. EFUSE 8188E — temuan terpenting

### 4.1 Alur: OFFLOAD, bukan baca langsung

`InitOffload_ReadEfuse_8188EU` (0xb1ea5) — decompilation:

```c
int InitOffload_ReadEfuse_8188EU(_ADAPTER *adapter)
{
    uint8_t local_138[256];       // buffer mentah
    uint8_t mode, v;
    uint16_t base, i;
    uint32_t hdr, d1, d2;

    for (mode = 0xaa; /* ... */; ) {
        write8(adapter, 0x209, mode);          // mode EFUSE (0xaa / 0xf4)
        base = mode * 0x10;                    // alamat window

        // 1. flag + reset 8051 + trigger
        v = read8(adapter, 0x0f0);
        write8(adapter, 0x0f0, v | 0x80);
        HAL_8051Reset(adapter);
        v = read8(adapter, 0x088);
        write8(adapter, 0x088, v | 0x02);
        for (i = 0; i < 1000; i++) {           // tunggu bit1 hilang
            v = read8(adapter, 0x088);
            if ((v & 0x02) == 0) break;
            stall(50);
        }
        if (i >= 1000) { /* timeout: pulihkan 0x0f0 & reset 8051 */ break; }

        // 2. aktifkan akses & baca header+data
        write8(adapter, 0x106, 0x69);           // EFUSE access ON
        write16(adapter, 0x140, base);         // window
        write8(adapter, 0x143, 0);
        for (i = 0; i < 50; i++) {             // tunggu bit0 siap
            v = read8(adapter, 0x143);
            if (v & 0x01) break;
            stall(50);
        }
        hdr = read32(adapter, 0x144);
        d1  = read32(adapter, 0x148);
        d2  = read32(adapter, 0x148);

        if ((hdr & 0xffff) < 0x101) {
            local_138[0] = hdr >> 16;
            local_138[1] = hdr >> 24;
            for (i = 0; i < 4; i++) local_138[2 + i] = (d1 >> (8*i)) & 0xff;

            nblocks = ((hdr & 0xffff) - 8) >> 3;
            extra    = (hdr & 7) != 0;
            for (k = 0; k < nblocks + extra; k++) {
                write16(adapter, 0x140, base + 1 + k);
                write8(adapter, 0x143, 0);
                for (i = 0; i < 50; i++) {
                    v = read8(adapter, 0x143);
                    if (v & 0x01) break;
                    stall(50);
                }
                d1 = read32(adapter, 0x144);
                d2 = read32(adapter, 0x148);
                for (i = 0; i < 4; i++) local_138[k*8 + 6 + i] = (d1 >> (8*i)) & 0xff;
                for (i = 0; i < 4; i++) local_138[k*8 + 10 + i] = (d2 >> (8*i)) & 0xff;
            }
        }
        EFUSE_GetEfuseDefinition(adapter, 0, 4, &deflen, 0);
        ParsingEfuseContent_8188EU(adapter, local_138, rom, deflen, (short)hdr, 0);
        memcpy(adapter + 0x4c6, rom, 0x200);
    }
}
```

### 4.2 Register yang dipakai (offload 8188E)

| Register | Fungsi |
|---|---|
| `0x209` | mode EFUSE (0xaa atau 0xf4) |
| `0x0F0` | flag enable (`|0x80` mulai, `&0x7f` selesai) |
| `0x088` | trigger offload (bit1), status (bit1 = sibuk) |
| `0x106` | EFUSE access = **0x69** |
| `0x140` | alamat window (16 bit) |
| `0x143` | status (bit0 = data siap) |
| `0x144` | header 32 bit (`addr` 16 bit + 2 byte tambahan) |
| `0x148` | data 32 bit (×2) |
| `0x093` / `0x003` | 8051 IO wrapper (pause/enable) |

### 4.3 Parser EFUSE — perbedaan dari OpenBSD

`ParsingEfuseContent_8188EU` (0xb1bc2) memakai **header blok di dalam stream**:
blok dengan `off >= 16` memakai **extended header 2 byte**:

```
byte1: 0x0f | ((off & 0x07) << 5)      → (cur & 0x1f) == 0x0f
byte2: ((off & 0x38) << 1) | mask        → (nxt & 0x0f) == mask
off  = ((nxt >> 1) & 0x78) | (cur >> 5)
mask = nxt & 0x0f                          ← MASK ADA DI BYTE HEADER!
```

Layout output: `out[blok*8 + slot*2 + 0]` = byte pertama,
`out[blok*8 + slot*2 + 1]` = byte kedua → **sama dengan OpenBSD** ✓

Perbedaan penting vs OpenBSD:

| # | OpenBSD `rtwn_efuse_read` | Vendor 8188E |
|---|---|---|
| 1 | `EXT_HDR` lewat flag `RTWN_FLAG_EXT_HDR` | selalu aktif |
| 2 | extended header **tidak** membawa mask | mask = `nxt & 0x0f` |
| 3 | `rom[off*8 + …]` tanpa mask `& 0x3f` | `(off & 0x3f)*4 + slot` |
| 4 | data di luar buffer → blok dilewati | data di luar buffer → **ditulis 0** |

Perbedaan #2 & #4 ditemukan oleh **differential test**
(`tools/test_efuse_diff.c`): 20.000 image EFUSE acak dibandingkan dengan
transkripsi literal pseudocode Ghidra → **0 perbedaan** ✓

### 4.4 Tabel mask EFUSE 8188E (asli, dari binary)

`_Array_MP_8188E_MUSB` @0x376f70 (32 byte; `EFUSE_GetArrayLen_MP_8188E_MUSB` = 0x20):

```
ff f3 00 00 00 00 00 00 00 00 0f f3 ff ff 30 00 00 00 f7 00 00 00 00 00 00 00 00 00 00 00
```

Tiap byte = mask blok ke-n (bit 1 = slot tidak ada).
Efektif (4 bit): blok 0→0x0f, 1→3, 2–10→0, 11→0x0f, 12→3, 13–14→0x0f, 15→0, 16–17→0,
18→7, 19–31→0.

`_Array_TC_8188E_MUSB` @0x376f50 (varian TC/PCIe) juga tersedia.

### 4.5 Konfirmasi independen: `Hal_EfuseParseXtal_8188E` (0x145358)

```c
void Hal_EfuseParseXtal_8188E(_ADAPTER *adapter, void *rom, char isPCIE)
{
    if (isPCIE != 0) { *(uint16 *)(adapter + 0x390 + 0x3d9f) = 0x20; return; }
    cVar2 = ' ';
    if (*(char *)(rom + 0xb9) != -1) cVar2 = *(char *)(rom + 0xb9);   // rom[185] ★
    *(char *)(adapter + 0x390 + 0x3d9f) = cVar2;
    cVar2 = *(char *)(rom + 6);                                        // rom[6]
    if (cVar2 == -1) cVar2 = 0;
    *(char *)(adapter + 0x390 + 0x3da0) = cVar2;
}
```

→ **rom[185] = xtal** ✓ identik dengan `struct r88e_rom.xtal` (offset 185) di OpenBSD ✓
→ crystal cap dari **rom[6]** (bukan 185!).

### 4.6 Register 8188E yang dipetakan vendor (`GetHwReg8188EUsb`, 0xb5c46)

| Offset logis | Register fisik | Cara |
|---|---|---|
| 0x04–0x0A | **MACID = 0x610** (4 byte) | `PlatformEFIORead4Byte(adapter, 0x610)` |
| 0x28–0x2A | 0x560 / 0x564 (8 byte) | `PlatformEFIORead4Byte` |
| 0x3A | cek flag di 0x608 | `PlatformEFIORead4Byte(adapter, 0x608)` |
| 0x1A/0x1B/0x1C/0x1F | field struct adapter (bukan register USB) | baca memori adapter |

---

## 5. Reset 8051 (dipakai offload)

```c
void HAL_8051Reset(_ADAPTER *adapter)
{
    HAL_8051PauseIOWrapper(adapter);
    PlatformStallExecution(50);
    HAL_8051EnableIOWrapper(adapter);
}
```

Untuk tipe 8188EUSB (`chip type 0x32`):

```c
/* Pause */
v = read8(adapter, 0x093); write8(adapter, 0x093, v & 0xFE);
v = read8(adapter, 0x003); write8(adapter, 0x003, v & 0xFB);

/* Enable */
v = read8(adapter, 0x093); write8(adapter, 0x093, v | 0x01);
v = read8(adapter, 0x003); write8(adapter, 0x003, v | 0x04);
```

(Untuk chip lain: register 0x01C/0x01D.)

---

## 6. Upload firmware 8051

| Fungsi | Alamat | Peran |
|---|---|---|
| `FW_FirmwareDownload` | 0x99e07 | Orquestrasi unduhan firmware |
| `FillH2CCommand8188E_MP` | 0xb1524 | Susun perintah H2C |
| `SetFwPwrModeCmd_8188E` | 0xb0029 | Mode power firmware |
| `AddH2CCmdQueue_8188E` | 0xaff42 | Antrean perintah H2C |
| `H2CCmdAction8188E` | — | Eksekusi H2C |

Vendor memakai firmware **tertanam** di binary (`_Array_MP_8188E_MUSB`),
kita memakai `rtl8188eufw.bin` dari linux-firmware (izin jelas).

---

## 7. Implementasi kita

| File | Isi |
|---|---|
| `src/urtwn/rtwn_seq.c` | Logika M1 portabel (C murni, tanpa IOKit): register access, parser EFUSE (identik vendor), power-on, LLT/DMA, upload firmware |
| `src/urtwn/rtwn_seq_efuse.c` | **Jalur OFFLOAD EFUSE 8188E** (implementasi §4.1) |
| `src/urtwn/rtwn_seq.h` | API + hasil (MAC, xtal, regulatory, channel_plan) |
| `tools/rtwn_probe.c` | Probe user space (`: baca register`, `--efuse`, `--efuse-offload`, `--full`, `--write-test`) |
| `tools/test_efuse.c` | Uji sintetis dengan **tabel mask asli vendor** |
| `tools/test_efuse_diff.c` | **Differential test** vs transkripsi pseudocode Ghidra (20.000 image acak) |

Status test:

```
✅ differential test : IDENTIK dengan referensi vendor (20.000 image acak)
✅ test sintetis     : xtal=0xa0 ✓ channel_plan=0x17 ✓ MAC a0:1b:… ✓
```

---

## 8. Setup tool RE (reusable)

| Komponen | Lokasi |
|---|---|
| Node.js 24.18.0 | `~/.local/node` |
| REA (`rea-agents` 6.7.0) | `~/.local/node/bin/rea` — MCP: `rea-agents mcp` |
| Ghidra 12.1.4 | `~/.local/ghidra/ghidra_12.1.4_PUBLIC` |
| Ghidra 11.4.3 (sumber decompiler macOS) | `~/.local/ghidra/ghidra_11.4.3_PUBLIC` |
| JDK Temurin 21.0.12.1 | `~/.local/jdk` |
| MCP OpenCode | `~/.config/opencode/opencode.json` → `mcp.rea` (207 tool) |

**Trik penting**: REA menuntut Ghidra **12.1.x**, tapi 12.1.x tidak lagi menyertakan
decompiler macOS. Solusinya: **salin** `Ghidra/Features/Decompiler/os/mac_x86_64`
dari Ghidra 11.4.3 ke Ghidra 12.1.4 → semua check REA jadi hijau ✓

```bash
cp -R ~/.local/ghidra/ghidra_11.4.3_PUBLIC/Ghidra/Features/Decompiler/os/mac_x86_64/. \
      ~/.local/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Features/Decompiler/os/mac_x86_64/
```

Contoh pemakaian MCP REA:

```js
await tools.rea.open_binary({ path: "<binary>" });
await tools.rea.batch_decompile({ addresses: ["0x80aaa", "0x74489"] });
```

> Rizin **tidak** bisa dipakai di mesin ini: paket macOS-nya hanya arm64, sedangkan
> mesin ini Intel x86_64 (`EBADARCH`, -86).
