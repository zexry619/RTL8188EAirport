# Urutan Inisialisasi 8188E USB (dari RE driver vendor)

Tanggal: 22 Okt 2026 · Sumber: decompilation Ghidra atas `RtWlanU.kext`
(11.958 simbol, tidak stripped).

> Dokumen ini adalah **peta jalan** untuk M1. Semua register & urutan di bawah
> berasal dari binary vendor, bukan dari dokumentasi pihak ketiga.

---

## 1. Fungsi master: `InitializeAdapter8188EUsb` (0xb3c4c)

Urutan panggilan (hasil decompilation, urutan preserved):

| # | Panggilan vendor | Arti | Status di kode kita |
|---|---|---|---|
| 1 | `MgntIsSupportRemoteWakeUp` | cek fitur | ⬜ |
| 2 | `InitCrystalFrequency_8188E` | baca xtal dari EFUSE | ✅ (EFUSE) |
| 3 | `CardDisableRTL8188EU` | nonaktifkan kartu | ⬜ |
| 4 | `Hal_ClearRsvdCtrl` | bersihkan reserved ctrl | ⬜ |
| 5 | `HalSetFWWoWlanMode8188E` | mode firmware | ⬜ |
| 6 | `SetFwRemoteWakeCtrlCmd_8188E` | remote wake | ⬜ |
| 7 | `SimpleInitializeAdapter8188EUsb` | init sederhana | ⬜ |
| 8 | `HALWOWL_ResetVar` | reset var WoWLAN | ⬜ |
| 9 | **`_InitPowerOn88EU`** | **power-on (tabel flow)** | ✅ `rtwn_flow_power_on_8188e` |
| 10 | **`InitLLTTable8188E`** | **tabel LLT** | ✅ `rtwn_llt_init_8188e` |
| 11 | `_InitRDGSetting_8188E` | RDG | ⬜ |
| 12 | `InitHalDm` | DM (dynamic mechanism) | ⬜ |
| 13 | **`FW_FirmwareDownload`** | **upload firmware 8051** | ⚠️ versi OpenBSD (perlu port H2C vendor) |
| 14 | `FW_InitializeHALVars` | var HAL | ⬜ |
| 15 | `PHY_MACConfig` | init MAC | ⬜ |
| 16 | `PHY_BBConfig` | init BB | ⬜ |
| 17 | `PHY_RFConfig8188E` | init RF | ⬜ |
| 18 | `_InitQueueReservedPage_8188E` | halaman reserved | ⬜ |
| 19 | `_InitTxBufferBoundary_8188E` | batas buffer TX | ⚠️ perlu cek |
| 20 | `_InitQueuePriority_8188E` | prioritas antrean | ⚠️ perlu cek |
| 21 | `_InitPageBoundary_8188E` | batas halaman | ⚠️ perlu cek |
| 22 | `_InitWMACSetting_8188E` | setting WMAC | ⬜ |
| 23 | `_InitAdaptiveCtrl_8188E` | adaptive ctrl | ⬜ |
| 24 | `_InitEDCA_8188EU` | EDCA | ⬜ |
| 25 | `_InitRetryFunction_8188E` | retry | ⬜ |
| 26 | `usb_AggSettingTx/RxUpdate_8188E` | agregasi USB | ⬜ (M2) |
| 27 | `_InitBeaconParameters_8188E` | beacon | ⬜ |
| 28 | `_InitAdhocWorkaroundParams_8188E` | workaround ad-hoc | ⬜ |
| 29 | **`MgntActSet_802_3_MAC_ADDRESS`** | **set MAC ke chip** | ⬜ (perlu: MAC dari EFUSE) |
| 30 | `ActSetWirelessMode` | mode wireless | ⬜ |
| 31 | **`InitOffload_EfusePatch_8188EU`** | patch EFUSE offload | ⬜ |
| 32 | `CamResetAllEntry` | reset CAM | ⬜ |
| 33 | `PHY_SetTxPowerLevel8188E` | daya tx | ⬜ |
| 34 | `MgntActSet_RF_State` | state RF | ⬜ |
| 35 | **`PlatformUsbEnableInPipes`** | **buka pipe RX USB** | ⬜ (M2) |
| 36 | `_InitRfPowerState_8188E` | state daya RF | ⬜ |
| 37 | `halrf_iqk_trigger` / `halrf_lck_trigger` | kalibrasi RF | ⬜ |
| 38 | `HAL_WifiOnlyConfig` | konfigurasi Wi-Fi only | ⬜ |
| 39 | `ResetWoLPara` | reset WoL | ⬜ |
| 40 | `PHY_SetRFEReg_8188E` | set reg RFE | ⬜ |
| 41 | `odm_dm_init` | init DM | ⬜ |
| 42 | `PlatformStartWorkItem` | mulai work item | ⬜ |

**Kebutuhan minimum untuk M1 (chip hidup & MAC terbaca):** #2 (EFUSE) → #9 (power-on)
→ #10 (LLT) → #13 (firmware) → #29 (set MAC).

---

## 2. Tabel "power flow" vendor

Driver mendeskripsikan urutan inisialisasi sebagai **tabel data statis** per chip.
Ada **9 tabel** untuk 8188E:

| Tabel | Alamat | Ukuran |
|---|---|---|
| `_rtl8188E_power_on_flow` | 0x2b6240 | 10 entri |
| `_rtl8188E_radio_off_flow` | 0x2b62a0 | — |
| `_rtl8188E_card_disable_flow` | 0x2b6300 | — |
| `_rtl8188E_card_enable_flow` | 0x2b63b0 | — |
| `_rtl8188E_suspend_flow` | 0x2b6460 | — |
| `_rtl8188E_resume_flow` | 0x2b6510 | — |
| `_rtl8188E_hwpdn_flow` | 0x2b65c0 | 6 entri |
| `_rtl8188E_enter_lps_flow` | 0x2b6670 | — |
| `_rtl8188E_leave_lps_flow` | 0x2b66f0 | — |

### Format entri `WL_PWR_CFG` (8 byte)

```
+0  u16  Register
+2  u8   BitMaskReg     (entri dilewati kalau 0)
+3  u16  Ext            (unaligned)
       Ext & 0x0f        = unit mask
       (Ext >> 4) & 0x0f = type mask
       Ext >> 12         = tipe perintah
+5  u8   AndMask
+6  u8   OrMask
+7  u8   padding
```

Tipe perintah:

| Type | Arti |
|---|---|
| 1 | **WRITE** : `reg = (reg & ~AndMask) \| (OrMask & AndMask)` |
| 2 | **POLL** : tunggu sampai `(reg & AndMask) == (OrMask & AndMask)` |
| 3 | **DELAY** : tunggu `reg` ms (atau `reg*1000` ms kalau `OrMask != 0`) |
| 4 | **END** |

Eksekutor: `HalPwrSeqCmdParsing` (0x9a6c4).
Alat dekoder kami: **`tools/decode_flow_table.py`**.

### `_rtl8188E_power_on_flow` (lengkap)

```
#  Reg    BMask Ext    Type  And/Or   aksi
0  0x0006 0xff  0x20ff POLL  0x02/0x02  tunggu (reg & 0x02) == 0x02   ← 8051 siap
1  0x00f2 0xff  0x10ff WRITE 0x10/0x00  reg &= ~0x10                  ← mode EFUSE
2  0x0002 0xff  0x10ff WRITE 0x03/0x00  reg &= ~0x03                  ← SYS_FUNC_EN
3  0x0026 0xff  0x10ff WRITE 0x80/0x80  reg |= 0x80                   ← AFE_XTAL
4  0x0005 0xff  0x10ff WRITE 0x80/0x00  reg &= ~0x80
5  0x0005 0xff  0x10ff WRITE 0x18/0x00  reg &= ~0x18
6  0x0005 0xff  0x10ff WRITE 0x01/0x01  reg |= 0x01
7  0x0005 0xff  0x20ff POLL  0x01/0x00  tunggu (reg & 0x01) == 0x00   ← 8051 selesai
8  0x0023 0xff  0x10ff WRITE 0x10/0x00  reg &= ~0x10                  ← LPLDO_CTRL
9  0x0074 0xff  0x101f WRITE 0x10/0x10  reg |= 0x10                   ← HWPDN
10 0xffff 0xff  0x40ff END
```

Implementasi: `src/urtwn/rtwn_flow.c` → `rtwn_flow_power_on_8188e()`.

### `_rtl8188E_hwpdn_flow`

```
0  0x001f WRITE reg &= ~0xff
1  0x0023 WRITE reg |= 0x10
2  0x0005 WRITE reg |= 0x02
3  0x0005 POLL  tunggu (reg & 0x02) == 0
4  0x0006 WRITE reg &= ~0x01
5  0x0005 WRITE reg |= 0x80
6  END
```

---

## 3. ⭐ Layout register EFUSE 8188E — penyebab kegagalan M1

Vendor `EFUSE_OneByteRead` (0x119c0b):

```c
PlatformIOWrite1Byte(adapter, 0x031, offset & 0xFF);              // address low
v = PlatformEFIORead1Byte(adapter, 0x032);
PlatformIOWrite1Byte(adapter, 0x032, (v & 0xFC) | ((offset >> 8) & 3)); // address high
v = PlatformEFIORead1Byte(adapter, 0x033);
PlatformIOWrite1Byte(adapter, 0x033, v & 0x7F);                    // clear busy
for (i = 0; i < 100 && (read(0x033) & 0x80); i++) stall(1000);      // tunggu busy clear
data = PlatformEFIORead1Byte(adapter, 0x030);                      // DATA
```

| Register | Fungsi 8188E | OpenBSD (92C-style) |
|---|---|---|
| 0x030 | **DATA** | address+status (ADDR di bit 8-17, VALID bit 31) |
| 0x031 | address low | — |
| 0x032 | address high (2 bit) | — |
| 0x033 | status (bit7 = busy) | — |

**OpenBSD menulis address ke 0x030 — pada 8188E itu register DATA, bukan address.**
Akibatnya pembacaan EFUSE versi OpenBSD **tidak pernah sukses** → MAC tidak pernah
terbaca → M1 gagal. Ini akar masalah sebenarnya (bukan encoding, bukan transfer USB).

Implementasi: `src/urtwn/rtwn_efuse_8188e.c` → `rtwn_seq_read_efuse_direct_8188e()`.

---

## 4. Jalur OFFLOAD EFUSE (alternatif)

`InitOffload_ReadEfuse_8188EU` (0xb1ea5) — mesin internal/8051 melakukan pembacaan:

```
write8(0x209, mode)            mode = 0xaa (atau 0xf4) → base = mode << 4 (= 0xAA0)
write8(0x0F0, v|0x80)         flag
reset 8051 (0x093/0x003)
write8(0x088, v|0x02)         trigger
poll 0x088 sampai bit1 hilang  (1000 × 50 ms)
write8(0x106, 0x69)           EFUSE access ON
write16(0x140, base)          window
write8(0x143, 0); poll bit0   (50 × 50 ms)
read32(0x144) = hdr, read32(0x148) = data
```

Mode/base: `GetInitialTxBufferBoundary88E` → 0xaa (default) / 0xf4.
Jumlah blok: `nblocks = ((hdr & 0xffff) - 8) >> 3`, `extra = (hdr & 7) != 0`.

Implementasi: `src/urtwn/rtwn_seq_efuse.c` → `rtwn_seq_read_efuse_offload()`.

---

## 5. Parser EFUSE — identik vendor (terverifikasi)

`ParsingEfuseContent_8188EU` (0xb1bc2). Dua perbedaan dari OpenBSD:

1. Extended header (blok ≥ 16) **membawa mask**: `mask = nxt & 0x0f`.
2. Data di luar buffer → **ditulis 0**, bukan dilewati.

Layout output: `out[blok*8 + slot*2 + 0]` = byte pertama, `+1` = byte kedua
(sama dengan OpenBSD).

**Verifikasi:** `tools/test_efuse_diff.c` membandingkan parser kita dengan
transkripsi literal pseudocode Ghidra pada **20.000 image EFUSE acak** →
**0 perbedaan** ✓

Tabel mask asli 8188E (`_Array_MP_8188E_MUSB` @0x376f70, 32 byte):

```
ff f3 00 00 00 00 00 00 00 00 0f f3 ff ff 30 00 00 00 f7 00 00 00 00 00 00 00 00 00 00 00
```

Konfirmasi independen: `Hal_EfuseParseXtal_8188E` (0x145358) membaca
**rom[185]** sebagai xtal → identik `struct r88e_rom.xtal` (offset 185) OpenBSD ✓
dan **rom[6]** sebagai crystal cap.

---

## 6. Init LLT 8188E (versi vendor)

`InitLLTTable8188E` (0xae125) — berbeda dari OpenBSD:

```
total = 0xAF (175)  [atau 0xFF untuk tipe chip tertentu]
untuk i = 1 .. page_count-1:
    write32(0x1E0, (i & 0xFF) | ((i-1) << 8) | 0x40000000)
write32(0x1E0, ((page_count-1) & 0xFF) << 8 | 0x400000FF)
untuk i = page_count .. total-1:
    write32(0x1E0, (i & 0xFF) | (page_count << 8) | 0x40000000)
write32(0x1E0, (page_count & 0xFF) | (total << 8) | 0x40000000)
```

`0x1E0` = `R92C_LLT_INIT` ✓ sama dengan OpenBSD; `0x40000000` = op WRITE ✓.

Implementasi: `rtwn_llt_init_8188e()`.

---

## 7. Reset 8051

```c
void HAL_8051Reset(adapter) {
    HAL_8051PauseIOWrapper(adapter);
    PlatformStallExecution(50);
    HAL_8051EnableIOWrapper(adapter);
}
```

Untuk tipe 8188EUSB (`chip type 0x32`):

```
Pause : read8(0x093) → write8(0x093, v & 0xFE)
        read8(0x003) → write8(0x003, v & 0xFB)
Enable: read8(0x093) → write8(0x093, v | 0x01)
        read8(0x003) → write8(0x003, v | 0x04)
```

(Untuk chip lain: register 0x01C/0x01D.)

---

## 8. Peta register 8188E yang dipakai vendor

| Offset | Nama (OpenBSD) | Peran |
|---|---|---|
| 0x000 | `R92C_SYS_ISO_CTRL` | isolasi power |
| 0x002 | `R92C_SYS_FUNC_EN` | enable blok (BB reset, CPUEN, ELDR) |
| 0x004 | `R92C_APS_FSMCO` | state MAC/host |
| 0x006 | `R92C_SYS_CLKR` | clock (bit1 = 8051 siap) |
| 0x01C | `R92C_RSV_CTRL` | reserved control (MCU reset 0x0800) |
| 0x01D | — | 8051 IO wrapper |
| 0x020/0x021/0x022 | `R92C_LDOA15/LDOV12D/LDOHCI12_CTRL` | LDO |
| 0x023 | `R92C_LPLDO_CTRL` | LDO (power) |
| 0x024 | `R92C_AFE_XTAL_CTRL` | crystal (bit7) |
| 0x026 | — | crystal ext (bit7) |
| 0x030 | — | **EFUSE data (8188E)** |
| 0x031/0x032/0x033 | — | **EFUSE addr lo/hi/status (8188E)** |
| 0x080 | `R92C_MCUFWDL` | firmware download |
| 0x088 | — | offload trigger/status |
| 0x0F0 | — | offload ctrl |
| 0x0F2 | — | EFUSE mode |
| 0x100 | `R92C_CR` | enable DMA/MAC |
| 0x106 | — | EFUSE access (0x69) |
| 0x140 | — | offload window |
| 0x143 | — | offload status |
| 0x144/0x148 | — | offload header/data |
| 0x1E0 | `R92C_LLT_INIT` | LLT page mapping |
| 0x209 | — | EFUSE offload mode (0xaa/0xf4) |
| 0x560/0x564 | — | register gabungan 8 byte |
| 0x608/0x610 | `R92C_MACID` | MAC address |
| 0x674/0x678 | — | 8 byte (0x560/0x564) |

---

## 9. Yang masih perlu di-RE (untuk M2 ke atas)

| Item | Fungsi vendor | Alamat |
|---|---|---|
| Upload firmware (H2C) | `FW_FirmwareDownload`, `FillH2CCommand8188E_MP`, `SetFwPwrModeCmd_8188E` | 0x99e07 / 0xb1524 / 0xb0029 |
| Pipe USB RX/TX | `PlatformUsbEnableInPipes`, `TxFillDescriptor8188EUsb`, `HalUsbEnableRx8188EUsb` | — |
| Batas halaman/buffer | `_InitTxBufferBoundary_8188E`, `_InitQueuePriority_8188E`, `_InitPageBoundary_8188E`, `GetInitialRQPValue88EU` | 0xb2f7a / 0xb2e7c |
| Init MAC/BB/RF | `PHY_MACConfig`, `PHY_BBConfig`, `PHY_RFConfig8188E` | — |
| Set MAC ke chip | `MgntActSet_802_3_MAC_ADDRESS` | — |
| Kalibrasi RF | `halrf_iqk_trigger`, `halrf_lck_trigger` | — |
