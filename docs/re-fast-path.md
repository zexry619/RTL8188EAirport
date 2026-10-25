# RE cepat: pakai driver vendor sebagai peta

> Ditemukan 23 Okt 2026. Driver vendor Realtek (`RtWlanU.kext`) yang berjalan
> di mesin ini **tidak di-strip**. Ini mengubah RE dari "bongkar buta" menjadi
> "baca peta".

## Kenapa ini cepat

| Fakta | Angka |
|---|---|
| Simbol global di binary | **10.302** |
| Fungsi HAL 8188E bernama | **224** |
| Panjang binary | 11,7 MB (x86_64) |

Contoh nama yang langsung terbaca:

```
TxFillDescriptor8188EUsb(_ADAPTER*, _RT_TCB*, u16, u16, u16, u16)
TxFillCmdDesc8188EUsb(...)
_InitEDCA_8188EU(_ADAPTER*)
SetKey8188EUsb(_ADAPTER*, uint, u8*, u8, uint, u8, u8)
PHY_RFConfig8188E(_ADAPTER*)
InitLLTTable8188E(_ADAPTER*, uint)
_WriteFW_8188E(_ADAPTER*, void*, uint)
HalUsbEnableTx8188EUsb(_ADAPTER*)
CmdSendPacket_8188E(...)
InsertEMContent_8188E(...)
```

Bahkan lapisan WPA-nya ada: `Supplicant_OnEAPOLKeyRecvd`,
`Authenticator_OnEAPOLKeyRecvd`, `AP_Setkey`, dst.

## Alat

```bash
./scripts/vendor-sym.sh              # semua fungsi 8188E
./scripts/vendor-sym.sh tx           # cari yang berhubungan TX
./scripts/vendor-sym.sh key          # key / WPA
./scripts/vendor-dis.sh _InitEDCA_8188EU 200   # disassembly per nama
```

`vendor-dis.sh` mencari alamat dari tabel simbol lalu memanggil `objdump`
dengan rentang yang tepat — tidak perlu menghitung offset manual.

Untuk fungsi besar, decompile dengan Ghidra headless (opsional):

```bash
# butuh Ghidra terpasang; hasilnya C-like, jauh lebih cepat dibaca
analyzeHeadless /tmp/ghidra_proj rtw -import RtWlanU.kext \
  -postScript DecompileFunction.java TxFillDescriptor8188EUsb
```

## Temuan pertama (langsung diterapkan)

`_InitEDCA_8188EU` @ `0xb33f9` menulis delapan register:

| Register | Nilai | Isi |
|---|---|---|
| `0x428` | `0x100a` | SPEC_SIFS |
| `0x63a` | `0x100a` | MAC_SPEC_SIFS |
| `0x514` | `0x100a` | SIFS_CCK |
| `0x516` | `0x100a` | SIFS_OFDM |
| **`0x500`** | **`0x2fa226`** | **EDCA_VO_PARAM** |
| **`0x504`** | **`0x5ea324`** | **EDCA_VI_PARAM** |
| **`0x508`** | **`0x5ea42b`** | **EDCA_BE_PARAM** |
| **`0x50c`** | **`0x00a44f`** | **EDCA_BK_PARAM** |

Empat yang ditebalkan **tidak pernah ditulis** driver kita. Tanpa parameter
EDCA, queue BE memakai nilai reset chip — dan queue BE itulah yang dipakai
frame data. Sudah ditambahkan di `rtwn_core_config_8188e()`.

## Target berikutnya (urut nilai)

| Fungsi vendor | Untuk apa | Dipakai di kita |
|---|---|---|
| `TxFillDescriptor8188EUsb` | field descriptor TX yang benar (terutama bit cipher/QSEL) | `rtwn_usb_build_tx_desc` |
| `SetKey8188EUsb` | jalur HW crypto / CAM (M5 yang belum ada) | belum ada — kita software CCMP |
| `HalUsbEnableTx8188EUsb` | urutan enable TX queue | `rtwn_core_config_8188e` |
| `InitLLTTable8188E` / `_WriteFW_8188E` | init DMA + firmware | `rtwn_seq_dma_init` / `rtwn_seq_load_firmware` |
| `Supplicant_OnEAPOLKeyRecvd` | alur 4-way handshake vendor | net80211 (sudah ada) |

## Aturan pakai

1. **Selalu** cari nama dulu (`vendor-sym.sh`), jangan langsung `objdump` alamat acak.
2. Bandingkan dengan **OpenBSD `urtwn`** — vendor dan OpenBSD sering berbeda;
   OpenBSD lebih ringkas, vendor lebih lengkap. Ambil yang sesuai jalur kita.
3. Kalau vendor dan OpenBSD berbeda, **vendor menang** untuk pertanyaan
   "apa yang benar-benar dibutuhkan chip ini" — karena vendor yang lulus di
   Windows/macOS.
4. Setiap konstanta yang diambil dari vendor dicatat di komentar kode
   (contoh: blok EDCA di `rtwn_init_8188e.c`).
