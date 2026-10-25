# `src/airport/` — Layer 4: frontend IO80211

Bridge antara `net80211` (Layer 3) dan **CoreWLAN** (milik Apple).

## Sumber

```
upstream/itlwm/AirportItlwm/            # laobamac/itlwm — GPL-2.0
upstream/itlwm/include/Airport/         # header IO80211 privat Apple
```

## File yang diadaptasi

| File upstream | Peran | Adaptasi |
|---|---|---|
| `AirportItlwm.cpp` | `IO80211Controller` | ganti panggil Intel `iwl_*` → `urtwn_*`/`rtwn_*` |
| `AirportItlwmInterface.cpp` | uplink ke CoreWLAN | sebagian besar reusable |
| `AirportItlwmSkywalkInterface.cpp` | jalur data modern | reusable |
| `AirportSTAIOCTL.cpp` | ioctl STA Apple | reusable |
| `AirportVirtualIOCTL.cpp` | ioctl virtual | reusable |
| `AirportAWDL.cpp` | AWDL | **tidak diimplementasikan** (chip tidak mendukung) |
| `IOPCIEDeviceWrapper.cpp` | pembungkus device | **ganti** → `USBBackend` (Layer 1) |

## Headroom yang harus dibuat sendiri

* `USBBackend.hpp` — pembungkus `IOUSBHostInterface`/`IOUSBHostPipe` dengan API
  yang menyerupai `IOPCIEDeviceWrapper` supaya `AirportItlwm.cpp` tidak banyak berubah.
* Mapping fungsi `iwl_*` → `urtwn_*` (daftar lengkap ada di
  `docs/porting-notes.md` §3 dan `docs/roadmap.md` M3–M4).

## Urutan pengerjaan yang disarankan

1. **Ethernet dulu** (`AirportItlwmEthernetInterface.cpp`): pola `IOEthernetController`
   jauh lebih sederhana dan bisa dipakai sebagai bring-up (M3) sebelum frontend
   IO80211 (M4).
2. **Lalu IO80211**: ubah `IOEthernetController` → `IO80211Controller`.

## Batasan

* `IO80211` = API privat Apple → harus build per versi macOS
  (`MinKernel`/`MaxKernel`), persis seperti AirportItlwm yang punya build
  Monterey/Sonoma/Sequoia terpisah.
* Header privat Apple **tidak boleh dipublikasikan** (lihat `docs/legal.md`).
