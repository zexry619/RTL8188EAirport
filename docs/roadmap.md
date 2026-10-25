# Roadmap

> **STATUS: DIJEDA (21 Okt 2026).** Prototipe M1 mencapai "driver match ke device
> USB & menjalankan init", namun dua kernel panic (use-after-free pada buffer
> transfer USB) menghentikan pengujian. Akar masalah & aturan yang lahir dari
> sana terdokumentasi di [`HANDOVER.md`](HANDOVER.md) §6.
> Langkah yang direkomendasikan berikutnya: **validasi protokol vendor request
> dari user space** (tidak mungkin panic kernel), baru lanjut ke kernel driver.

Estimasi assumes: osmotic **1 orang** yang sudah nyaman dengan C, IOKit, dan 802.11.
Untuk pemula, kalikan dengan 2–3×.

---

## Peta milestone

| M | Nama | Output yang bisa didemokan | Estimasi | Status |
|---|---|---|---|---|
| M0 | Toolchain & binding | Kext bind ke dongle, log properti USB, `kextutil` lolos | 1–3 hari | **selesai (scaffold)** |
| M1 | Firmware + EFUSE + RF init | Upload `rtl8188eufw.bin`, baca chip ID & MAC dari EFUSE, inisialisasi RF | 2–4 minggu | **kode siap; jalur EFUSE offload ditemukan lewat RE vendor** ([`docs/milestone-m1.md`](milestone-m1.md), [`docs/vendor-reverse.md`](vendor-reverse.md)) |
| M2 | TX/RX mentah + scan | Probe request terkirim, beacon diterima, SSID tercetak di log | 2–6 minggu | belum |
| M3 | net80211 attach | `ieee80211_ioctl` jalan; join network **terbuka** | 2–4 minggu | belum |
| M4 | Frontend IO80211 | Menu Wi-Fi macOS menampilkan daftar SSID | 3–6 minggu | belum |
| M5 | WPA2 + DHCP | Terhubung ke WPA2-PSK, dapat IP, internet jalan | 4–8 minggu | belum |
| M6 | Stabilitas | Sleep/wake, reconnect, uptime 24 jam, throughput stabil | 4–8 minggu | belum |
| M7 | Polish | Firmware, Info.plist per-OS, packaging, dokumentasi | 2–4 minggu | belum |

**Total realistis: 4–8 bulan** (M0→M5) untuk yang berpengalaman; 8–12+ bulan sampai stabil.

---

## Gerbang go/no-go

| Gerbang | Kriteria | Kalau gagal |
|---|---|---|
| **G1 (akhir M1)** | Chip ID terbaca `0x8188`/8188EUS + MAC EFUSE valid | Curiga firmware/EFUSE tidak cocok → re-check `r92creg.h` & `rtwn_efuse_read` |
| **G2 (akhir M2)** | Scan menemukan ≥ 5 SSID dari router rumah | Radio/RF init salah → bandingkan dengan `rtwn` + `rtw88` Linux (EFUSE offset, channel, power) |
| **G3 (akhir M3)** | Join network open (tanpa password) berhasil |(net80211↔urtwn) kontrak ioctl salah → audit `ieee80211_ioctl.c` |
| **G4 (akhir M4)** | Menu Wi-Fi macOS menampilkan SSID | IO80211 KPI salah → bandingkan dengan `AirportItlwm` |
| **G5 (akhir M5)** | WPA2 + DHCP + browsing jalan | Handshake 4-way gagal → cek key handling (`rtwn_set_key`, `ieee80211_ioctl(KEY)`) |

> **Aturan main:** kalau **G2** belum tembus dalam ~8–10 minggu kerja efektif, berhenti dan
> pindah ke AX200 + `AirportItlwm-Sequoia` (hasil praktis identik, biaya ±$15).
> Proyek tetap dilanjutkan sebagai pembelajaran, tapi jangan dibiarkan mengganggu sistem harian.

---

## Rincian per milestone

### M0 — Toolchain & binding ✅ (scaffold)
* [x] Toolchain tervalidasi: `clang` + MacKernelSDK + `-DKERNEL=1 -DKERNEL_PRIVATE=1`
* [x] `scripts/fetch-upstream.sh` (itlwm, OpenBSD urtwn, AirPort-RTW, MacKernelSDK, firmware)
* [x] `scripts/build-m0.sh` (compile → link → validasi plist/Mach-O → ad-hoc sign)
* [x] Kext `RTL8188EUHello` match `IOUSBHostDevice` 9047:268
* [x] `scripts/check-device.sh` (deteksi konflik dengan `RtWlanU`)
* [ ] Jalankan load M0 di mesin (perlu nonaktifkan `RtWlanU` dulu)
* [ ] Dokumentasikan output log ke `docs/testing.md`

### M1 — Firmware + EFUSE + RF  → detail: [`docs/milestone-m1.md`](milestone-m1.md)

**Sudah ditulis** (`src/urtwn/rtwn_m1.cpp`): akses register via vendor request,
baca EFUSE/ROM 88E (MAC di offset 215), power-on, DMA/LLT init, upload firmware.
Belum: `rtwn_fw_cmd` (H2C mailbox — dibutuhkan mulai M2/M5), `mac_init`/`bb_init`/
`rf_init` (tabel register panjang, masuk M2).

1. ~~Port `rtwn_fw_cmd()` → vendor command USB (control transfer).~~
2. Port `rtwn_efuse_read()` → baca EFUSE via `usbd_do_request` (0x87/0x88/0x89 vendor requests).
3. Port `rtwn_read_chipid()` → dekode `0x8188`/`0x8189` dari register.
4. Port `rtwn_read_rom()` → baca MAC address (EFUSE offset 0xA?).
5. Port `urtwn_attach()` → `rtwn_rf_*_init` sesuai chip (r92c: `rtwn_r92c_read_rom`).
6. **Deliverable:** log `chipid`, `mac`, `antenna`, `regulatory domain`.
7. **Test:** bandingkan MAC hasil baca EFUSE dengan MAC di router (DPID) & label perangkat.

### M2 — TX/RX + scan
1. Port `urtwn_alloc_tx_list/alloc_rx_list` → alokasi `IOBufferMemoryDescriptor` + queue `IOUSBHostPipe`.
2. Port `urtwn_tx_data`/`urtwn_rx_data` + framing (header `0x42`, `0x50`, `0x51` untuk USB).
3. Implementasi `ieee80211_radiotap` pada RX.
4. Scan: set channel, kirim probe request, kumpulkan SSID/BSSID/RSSI.
5. **Deliverable:** daftar SSID tercetak di kernel log.

### M3 — net80211
1. Integrasikan `itl80211` (34 file) + `compat.cpp` ke build.
2. Daftar `ieee80211_com` dari `urtwn` (media change, scan, join, key, power).
3. Handler `ieee80211_ioctl()` lengkap.
4. **Deliverable:** `ifconfig`/log menunjukkan interface 802.11, join open network.

### M4 — Frontend IO80211
1. Adaptasi `AirportItlwm.cpp` → controller IO80211.
2. `USBBackend` (pembungkus `IOUSBHostInterface`) menggantikan `IOPCIEDeviceWrapper`.
3. `AirportItlwmInterface` → mapping `setSSID`, `scan`, `setKey`, `getPower`.
4. **Deliverable:** menu Wi-Fi menampilkan SSID (belum tersambung).

### M5 — WPA2 + DHCP
1. Pastikan EAPOL frame diteruskan dua arah (Apple WCL melakukan 4-way).
2. Key management: `rtwn_set_key` (WEP/WPA-PTK/TKIP/CCMP) via vendor command.
3. **Deliverable:** DHCP & browsing.

### M6 — Stabilitas
1. `setPowerState()`; reload firmware saat wake dari sleep.
2. Reconnect otomatis, roaming, watchdog timeout.
3. Uptime 24 jam + soak test.

### M7 — Polish
1. `Info.plist` per versi macOS (`MinKernel`/`MaxKernel`).
2. Build script reproducible (fetch + patch + compile).
3. Dokumentasi + video demo.

---

## Risiko & penyangga

| Risiko | Dampak | Penyangga |
|---|---|---|
| Bug RX stall (seperti RTL8188EE project) | Milestone M2–M3 macet | Sisakan `RtWlanU` sebagai fallback; pivot ke AX200 |
| ABI IO80211 berubah | M4–M5 jadi sia-sia | Build per-OS; kompatibilitas ke belakang tidak dijamin |
| Throughput rendah | 30–60 Mbps | Diterima; chip memang 1x1 |
|lisensi GPL + header Apple | Distribusi bermasalah | Clean-room; jangan publikasikan header Apple |
| ESP penuh | Tidak bisa install | ✅ SELESAI: ESP 512 MB (`EFI-BIG`, sisa 477 MB) |
