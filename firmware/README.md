# Firmware

## `rtl8188eufw.bin`

| Item | Nilai |
|---|---|
| Ukuran | 15.262 byte |
| sha256 | `2ff74315287529dec2e50eb57d6e0c97d2116f28ae166773ccdf93b6360000c4` |
| Sumber | linux-firmware (`rtlwifi/rtl8188eufw.bin`) |
| Lisensi | Lisensi firmware Realtek (`LICENCE.rtlwifi_firmware`) —Umumnya boleh didistribusikan ulang, **verifikasi sebelum rilis** |
| Diperlukan oleh | `urtwn_load_firmware()` (OpenBSD `if_urtwn.c` baris ~2137) |

Ambil dengan:

```bash
curl -L -o firmware/rtl8188eufw.bin \
  https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtlwifi/rtl8188eufw.bin

shasum -a 256 firmware/rtl8188eufw.bin
# harus: 2ff74315287529dec2e50eb57d6e0c97d2116f28ae166773ccdf93b6360000c4
```

Alternatif: paket OpenBSD `urtwn-firmware` (`sysutils/urtwn-firmware`) berisi file yang sama.

## Catatan

* Firmware **tidak** diambil dari paket Realtek untuk macOS (`StatusBarApp`), karena
  lisensinya tidak jelas untuk redistribusi.
* Binary vendor ternyata **menanam** firmware 8188E sebagai array C
  (symbol `_Array_MP_8188E_MUSB` pada `RtWlanU`) — pendekatan kita (file terpisah,
  dari linux-firmware) lebih bersih dan legal.
* OpenBSD `urtwn` memuat firmware dengan method `H2C` (host-to-card) per halaman 512 byte;
  itu yang diimplementasikan `urtwn_fw_loadpage()`.
* Ukuran firmware hanya ~15 KB → **tidak** menjadi masalah ukuran ESP (sisa ±4,8 MB).
