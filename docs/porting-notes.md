# Catatan Porting

Rincian teknis: apa yang harus diubah dari kode OpenBSD agar bisa jalan di XNU.

---

## 1. Flag build yang sudah tervalidasi

Kext **tidak bisa** dibangun dengan header Kernel.framework biasa — error
`redefinition of 'IOUSBHostDevice'` karena header `IOKit/usb/IOUSBHostDevice.h`
meng-`#include <USBDriverKit/IOUSBHostDevice.h>` ketika `TARGET_OS_OSX` truthy, sementara
isi DriverKit itu adalah kelas user-space.

Solusinya: pakai **MacKernelSDK** + definisi kernel:

```bash
clang++ -mkernel -std=c++17 -fno-rtti -fno-exceptions -fno-common -nostdinc++ \
        -DKERNEL=1 -DKERNEL_PRIVATE=1 \
        -I vendor/MacKernelSDK/Headers \
        -c src/m0-hello/RTL8188EUHello.cpp -o build/RTL8188EUHello.o

ld -r -arch x86_64 -nostdlib -undefined dynamic_lookup \
   -L vendor/MacKernelSDK/Library/x86_64 -lkmod \
   -o build/RTL8188EUHello.kext/Contents/MacOS/RTL8188EUHello build/RTL8188EUHello.o
```

> `KERNEL=1` membuat blok `#if !KERNEL` pada header DriverKit dilewati → tidak ada
> redefinisi kelas. `KERNEL_PRIVATE=1` membuka API privat yang dibutuhkan driver Wi-Fi.

Verifikasi lokal yang tersedia di macOS 15 (Sequoia):

```bash
plutil -lint <kext>/Contents/Info.plist      # Info.plist valid
file <kext>/Contents/MacOS/<nama>            # harus: Mach-O 64-bit kext bundle x86_64
codesign --verify --deep --strict <kext>     # signature ok
```

> Catatan: `kextutil -n` (dry-run) **tidak lagi didukung** — pada macOS 15 `kextutil`
> hanyalah wrapper `kmutil` dan hanya menerima mode tertentu. Validasi fullest
> terjadi saat `kmutil load` (butuh sudo + SIP longgar).

---

## 2. Permukaan API OpenBSD di `if_urtwn.c`

53 kemunculan `usbd_*` — inilah inti pekerjaan Layer 1.

| OpenBSD | jumlah | Pemetaan macOS |
|---|---|---|
| `usbd_xfer` | 6 | `IOUSBHostPipe::io()` + `IOUSBHostCompletion` |
| `usbd_is_dying` | 5 | cek flag `terminate` pada provider |
| `usbd_status` | 4 | parameter `status` pada callback completion |
| `usbd_pipe` | 4 | `IOUSBHostPipe` pointer |
| `usbd_transfer` | 3 | `IOUSBHostPipe::controlRequest()` (sinkron) |
| `usbd_setup_xfer` | 3 | `IOUSBHostPipe::io()` (tidak ada objek transfer terpisah) |
| `usbd_ref_incr` / `usbd_ref_decr` / `usbd_ref_wait` | 6 | `IORetain` / `IORelease` |
| `usbd_open_pipe` / `usbd_close_pipe` | 2+2 | `IOUSBHostDevice::createPipe()` / `IOUSBHostPipe::close()` |
| `usbd_alloc_xfer` | 2 | `IOBufferMemoryDescriptor` per transfer |
| `usbd_alloc_buffer` | 2 | `IOBufferMemoryDescriptor` |
| `usbd_free_xfer` | 2 | `release()` pada buffer |
| `usbd_do_request` | 2 | `IOUSBHostPipe::controlRequest()` (control) |
| `usbd_abort_pipe` | 1 | `IOUSBHostPipe::abort()` |
| `usbd_clear_endpoint_stall_async` | 1 | `IOUSBHostPipe::clearStall(true)` |
| `usbd_interface2endpoint_descriptor` | 1 | `StandardUSB::getNextEndpointDescriptor()` |
| `usbd_get_interface_descriptor` | 1 | `IOUSBHostInterface::getInterfaceDescriptor()` |
| `usbd_get_xfer_status` | 1 | parameter `status` pada callback completion |
| `usbd_device` / `usbd_interface` | 2 | chain `getProvider()` |

API lain yang muncul (jumlah kecil, tetapi wajib ada):

* `m_len` (3) — field header mbuf; `itl80211/compat.cpp` sudah menyediakan wrapper mbuf.
* `bpf_mtap` (2) — BPF tap radiotap; ganti dengan penulisan langsung ke header mbuf.
* `timeout_add` / `timeout_set` (2+2) — timer; ganti `IOService::setTimeoutHandler` atau
  `thread_call` (lihat `itl80211/compat.cpp`).
* `memcpy` / `free` — standar.

---

## 3. Peta fungsi `if_urtwn.c` → milestone

| Baris | Fungsi | Milestone | Catatan porting |
|---|---|---|---|
| 432 | `urtwn_match` | M0 | Ganti device matching OpenBSD (`device_add`) → personality IOKit |
| 444 | `urtwn_attach` | M1 | Init device: baca descriptor, set config, jalankan `urtwn_power_on` |
| 521 | `urtwn_detach` | M1 | Teardown |
| 552 | `urtwn_open_pipes` | M2 | Bulk IN/OUT |
| 620 | `urtwn_close_pipes` | M2 | |
| 639–717 | `urtwn_alloc_rx_list` / `free` / `alloc_tx_list` / `free` | M2 | Buffer memory descriptor + transfer queue |
| 730 | `urtwn_task` | M2 | Task queue OpenBSD → `IOCommandGate` / workloop |
| 752/773 | `urtwn_do_async` / `urtwn_wait_async` | M2 | Callback vendor command (firmware, EFUSE) |
| 785–871 | `urtwn_write_region_1`, `urtwn_write_1/2/4`, `urtwn_read_*` | M1 | Register access via control transfer |
| 872 | `urtwn_llt_write` | M1 | TX descriptor ring |
| 891–939 | `urtwn_calib_*` | M1 | Kalibrasi (iq/loopback) |
| 940–968 | `urtwn_scan_*` | M2 | Submit/collect scan |
| 971–1013 | `urtwn_newstate`, `urtwn_updateslot`, `urtwn_updatedca` | M3 | State machine hooks net80211 |
| 1033–1114 | `urtwn_set_key` / `delete_key` | M5 | Vendor command key |
| 1116 | `urtwn_ccmp_decap` | M5 | Dekapsulasi CCMP untuk hardware offload |
| 1162 | `urtwn_rx_frame` | M2 | Parse frame USB → mbuf + radiotap |
| 1300 | `urtwn_rxeof` | M2 | Handler EOF bulk IN |
| 1418 | `urtwn_txeof` | M2 | Handler EOF bulk OUT |
| 1449/1552 | `urtwn_tx_fill_desc` / `_gen2` | M2 | TX descriptor format (chip 8188EUS = gen2) |
| 1634 | `urtwn_tx` | M2 | |
| 1750 | `urtwn_ioctl` | M3 | `ieee80211_ioctl` wrapper |
| 1768–2033 | `urtwn_r92c/r92e/r88e/r88f_power_on` | M1 | **8188EUS memakai jalur `r88e`** (`URTWN_DEV_8188EU` → `RTWN_CHIP_88E`) |
| 2035–2079 | `urtwn_llt_init`, `urtwn_auto_llt_init` | M1 | |
| 2080–2136 | `urtwn_fw_loadpage`, `urtwn_load_firmware` | M1 | Upload `rtl8188eufw.bin` |
| 2137 | `urtwn_dma_init` | M1 | |
| 2248 | `urtwn_aggr_init` | M6 | Agregasi TX/RX |
| 2301 | `urtwn_mac_init` | M1 | |
| 2331 | `urtwn_bb_init` | M1 | Baseband init |
| 2463 | `urtwn_burstlen_init` | M2 | Burst length USB |
| 2504 | `urtwn_power_on` | M1 | Urutan power-on |

**Kandidat utama "hemat waktu":** `urtwn_r92c_power_on` + `urtwn_load_firmware` +
`urtwn_dma_init` + `urtwn_bb_init` = inti inisialisasi chip. Kalau M1 mentok di sini,
bandingkan dengan `rtwn.c` (OpenBSD) dan `rtl8xxxu` (Linux).

---

## 4. Struktur data yang perlu diporting

`struct urtwn_softc` (baris 176) dan `struct rtwn_softc` (`rtwnvar.h`) berisi:

* `struct device *` / `struct usb_interface *` → ganti `IOUSBHostInterface *`
* `struct ieee80211com *` → **tetap** (itlwm menyediakan `ieee80211_com` untuk Darwin)
* `struct ifnet *` → `itl80211/compat.cpp` sudah mengadaptasi `ifnet` minimal ke Darwin
* `struct mbuf *` → mbuf BSD; adaptasi via `compat.cpp`
* `struct usb_xfer *` → `IOMemoryDescriptor` + `IOUSBHostCompletion` + `IOUSBHostPipe`
* `unsigned char *` firmware buffer → `IOBufferMemoryDescriptor`

---

## 5. Struct & fungsi yang harus cocok dengan `net80211` (itlwm)

`urtwn` memanggil API net80211 berikut — pastikan versi itlwm menyediakan semuanya:

`ieee80211_node`, `ieee80211_frame`, `ieee80211_key`, `ieee80211_state`,
`ieee80211_release_node`, `ieee80211_inputm`, `ieee80211_has_qos`, `ieee80211_get_qos`,
`ieee80211_get_hdrlen`, `ieee80211_amrr` (+ `ieee80211_amrr_node_init`,
`ieee80211_amrr_choose`), `ieee80211_up_to_ac`, `ieee80211_set_link_state`,
`ieee80211_set_key`, `ieee80211_delete_key`, `ieee80211_new_state`,
`ieee80211_get_txkey`, `ieee80211_get_rxkey`, `ieee80211_find_rxnode`,
`ieee80211_encrypt`, `ieee80211_rxinfo`, `ieee80211_radiotap_header`.

Semua ini berasal dari `itl80211/openbsd/net80211/` — **harus dicek satu per satu**,
karena itlwm could've Trimmed sebagian API yang tidak dipakai Intel.

---

## 6. Checklist "yang sering lupa"

* [ ] `MinKernel`/`MaxKernel` untuk setiap build (IO80211 ABI per versi macOS)
* [ ] `OSBundleLibraries` versi KPI yang benar (`com.apple.kpi.iokit`, `libkern`, `bsd`, `dsep`)
* [ ] Ad-hoc signing (`codesign -f -s -`) agar `kextutil`/`kmutil` mau memuat
* [ ] `CFBundleIdentifier` unik, tidak bentrok dengan `com.realtek.driver.RtWlanU`
* [ ] Driver dimatikan otomatis saat vendor kext aktif (cek di `start()`: `if (vendorPresent) return kIOReturnExclusiveAccess;`)
* [ ] Semua alokasi `IOBufferMemoryDescriptor` di-`release` (leak = panic setelah berjam-jam)
* [ ] Semua callback USB di-cancel saat `stop()` (menunggu `IORelease` blocking = deadlock)
* [ ] `setPowerState()` mengembalikan `IOPMAckImplied`/`IOPMAckGuard` dengan benar
* [ ] Tidak ada printf ke stdout di kernel → pakai `IOLog` / `OSReport`
