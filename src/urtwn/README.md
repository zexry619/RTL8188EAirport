# `src/urtwn/` — Layer 2: driver hardware Realtek

Berisi port dari **OpenBSD** (lisensi ISC):

| File upstream | Asal | Baris | Peran |
|---|---|---|---|
| `if_urtwn.c` | `openbsd/src` → `sys/dev/usb/if_urtwn.c` | 2.603 | USB attach, firmware, EFUSE, RF, TX/RX, scan |
| `rtwn.c` | `openbsd/src` → `sys/dev/ic/rtwn.c` | 3.424 | Kode bersama: `rtwn_fw_cmd`, `rtwn_efuse_read`, `rtwn_read_chipid`, key, RA, AMRR |
| `rtwnvar.h` | `openbsd/src` → `sys/dev/ic/rtwnvar.h` | — | `struct rtwn_softc` |
| `r92creg.h` | `openbsd/src` → `sys/dev/ic/r92creg.h` | — | Register map Realtek |

Sumber sudah diunduh oleh `scripts/fetch-upstream.sh` ke `upstream/openbsd/`.
Salin ke folder ini saat mulai porting (Jenkins style: salin, lalu patch).

## Status

⛔ Belum ada file di sini. Rencana:

### Rencana kerja

1. **Salin** file upstream ke `src/urtwn/`.
2. **Ganti header** BSD dengan header kernel:
   * `<sys/param.h>`, `<sys/mbuf.h>`, `<sys/systm.h>`, `<machine/bus.h>`, `<dev/usb/usb.h>` → `<IOKit/IOKitLib.h>` + shim dari `src/usb/`.
   * `<net80211/ieee80211_var.h>` → header net80211 dari `src/net80211/`.
3. **Ganti `struct device`/`struct usb_interface`** → `IOUSBHostInterface *`.
4. **Ganti fungsi `usbd_*`** → shim di `src/usb/usb_compat.h`.
5. **Ganti timer/locking** → `src/usb/bsd_compat.h` (lihat `itl80211/compat.cpp` untuk contoh).
6. **Daftarkan personality** untuk `IOUSBHostInterface` + `idVendor 9047` / `idProduct 268`.

### Checklist per milestone

* [ ] M1: `urtwn_attach` jalan; `rtwn_read_chipid` & `rtwn_efuse_read` benar
* [ ] M1: `urtwn_load_firmware` meng-upload `rtl8188eufw.bin`
* [ ] M2: pipe bulk IN/OUT; `urtwn_rx_frame` mem-parse frame
* [ ] M2: `urtwn_tx_fill_desc_gen2` (8188EUS = gen2)
* [ ] M3: `ieee80211_ioctl` terhubung ke net80211
* [ ] M5: `urtwn_set_key` / `rtwn_set_key`

### Catatan penting

* **Chip 8188EUS memakai jalur `r88e`** — di `if_urtwn.c`, entri
  `URTWN_DEV_8188EU(TPLINK, RTL8188EUS)` memberi flag `RTWN_CHIP_88E`.
  Jadi yang dipakai: `urtwn_r88e_power_on`, `rtwn_r88e_read_rom`.
  Jalur `r92c`/`r92e`/`r88f` tidak relevan untuk chip ini.
* Tabel device **tidak perlu ditambah**: `2357:010c` sudah ada di `urtwn_devs[]`.
* Semua fungsi `rtwn_*` ada di `rtwn.c` yang **tidak** mengandung referensi PCI
  (sudah diverifikasi) → tidak perlu memotong kode PCIe.
