# Arsitektur

Dokumen ini menjelaskan layering, alur data, dan kontrak antarmuka tiap lapisan.

---

## 1. Prinsip

1. **Jangan tulis stack 802.11 dari nol.** `net80211` OpenBSD sudah diport ke Darwin oleh
   `itlwm` (34 file). Kita hanya bridging.
2. **Jangan tulis driver chipset dari nol.** `urtwn` sudah mendukung chip kita
   persis (USB ID `2357:010c`). Kita hanya mengganti transport USB-nya.
3. **Jangan tulis frontend IO80211 dari nol.** `AirportItlwm` sudah bridging
   net80211↔CoreWLAN. Kita ganti bagian Intel dengan bagian `urtwn`.
4. **Pakai driver vendor sebagai oracle, bukan sumber.** Bandingkan hasil (scan list,
   EFUSE, RSSI) — jangan menyalin binary-nya.

---

## 2. Diagram lapisan

```
┌──────────────────────────────────────────────────────────────────────┐
│ USER SPACE (milik Apple, tidak kita tulis)                           │
│   CoreWLAN.framework  →  menu Wi-Fi, airportd, locationd, AWDL      │
│   WPA2/EAPOL ditangani Apple (WCL) — BUKAN oleh driver kita          │
└───────────────▲──────────────────────────────────────────────────────┘
                │  IO80211 KPI (Apple private API)
                │  IO80211Interface / IO80211Controller
┌───────────────┴──────────────────────────────────────────────────────┐
│ LAYER 4 — src/airport/   (frontend; adaptasi laobamac/itlwm)        │
│   • IO80211Controller : terdaftar ke IO80211Family stack              │
│   • IO80211Interface  : uplink ke CoreWLAN (scan/join/keys/status)   │
│   • SkywalkInterface  : jalur data modern (jika dipakai)             │
│   • STA IOCTL         : ioctl private Apple (apple80211_ioctl.h)    │
│   • AWDL              : stub — TIDAK diimplementasikan (lihat §6)    │
├──────────────────────────────────────────────────────────────────────┤
│ LAYER 3 — src/net80211/  (OpenBSD net80211 via itlwm, ISC)           │
│   ieee80211_ioctl()  : kontrak driver↔stack (scan/join/key/rate)     │
│   state machine      : AUTH/ASSOC/4-way handoff ke WCL                │
│   ratectl (AMRR)     : urutan kecepatan link                          │
├──────────────────────────────────────────────────────────────────────┤
│ LAYER 2 — src/urtwn/  (OpenBSD urtwn + rtwn, ISC)                   │
│   urtwn_attach/…     : USB, firmware, EFUSE, RF, TX/RX ring, scan    │
│   rtwn_*             : fw_cmd, efuse, chipid, key, RA, AMRR          │
├──────────────────────────────────────────────────────────────────────┤
│ LAYER 1 — src/usb/  (kita tulis: shim transport)                    │
│   usbd_xfer/usbd_pipe/usbd_do_request/usbd_setup_xfer/…             │
│        ↓  dipetakan ke                                                  │
│   IOUSBHostDevice / IOUSBHostInterface / IOUSBHostPipe              │
└──────────────────────────────────────────────────────────────────────┘
              firmware/rtl8188eufw.bin  (15.262 B)
```

---

## 3. Kontrak antar lapisan

### 3.1 LAYER 1 — shim USB (kita tulis)

OpenBSD memakai `struct usb_interface` + `usbd_*`. Di macOS modern kita memakai
`IOUSBHostInterface` + `IOUSBHostPipe`.

API OpenBSD yang muncul di `if_urtwn.c` (53 kemunculan) dan pemetaannya:

| OpenBSD | macOS | Catatan |
|---|---|---|
| `usbd_get_interface_descriptor` | `IOUSBHostInterface::getInterfaceDescriptor()` | Deskriptor interface |
| `usbd_interface2endpoint_descriptor` | `StandardUSB::getNextEndpointDescriptor()` | Bulk IN/OUT |
| `usbd_open_pipe` / `usbd_close_pipe` | `IOUSBHostDevice::createPipe()` / `IOUSBHostPipe::close()` | Pipe bulk |
| `usbd_alloc_xfer` / `usbd_setup_xfer` / `usbd_xfer` | `IOUSBHostPipe::io(buf, len, completion, timeout)` | Queue transfer asinkron (bisa paralel: OUT 8×, IN) |
| `usbd_alloc_buffer` / `usbd_free_xfer` | `IOBufferMemoryDescriptor` + `release()` | |
| `usbd_do_request` (control) | `IOUSBHostPipe::controlRequest(...)` / `IOUSBHostDevice::deviceRequest(...)` | Control transfer (vendor request firmware/EFUSE) |
| `usbd_transfer` (legacy sync) | `IOUSBHostPipe::controlRequest(...)` sinkron | |
| `usbd_status`, `usbd_get_xfer_status` | callback `IOUSBHostCompletion` → `(owner, param, status, bytes)` | |
| `usbd_abort_pipe`, `usbd_clear_endpoint_stall_async` | `IOUSBHostPipe::abort()` / `clearStall(true)` | |
| `usbd_is_dying` | cek flag `terminate` pada provider | |
| `usbd_ref_incr/decr`, `usbd_ref_wait` | `IORetain/Release` | |
| `usbd_device`, `usbd_interface` | `getProvider()` chain | |

**Keputusan penting:** gunakan stack USB **modern** (`IOUSBHostInterface`), bukan legacy
`IOUSBInterface` yang dipakai `RtWlanU`. Alasannya: legacy sudah deprecated, dan
`IOUSBHostPipe` menyediakan queue transfer (USB 2.0 burst) → throughput jauh lebih baik.
Konsekuensi: `RtWlanU` dan driver kita tidak bisa aktif bersamaan (lihat `docs/testing.md`).

### 3.2 LAYER 2 — hardware

`if_urtwn.c` (2.603 baris) + `rtwn.c` (3.424 baris) = **±6.000 baris C** yang harus diport.
Yang perlu diganti:
* header BSD (`sys/mbuf.h`, `sys/param.h`, `machine/bus.h`, `dev/usb/usb.h`) → header kernel;
* `struct ifnet` → `net80211` versi itlwm sudah menyediakan `ieee80211_com` +
  `ieee80211_node` (port-nya sudah adapting `ifnet` ke Darwin);
* `timeout_add/timeout_set` → `IOService::setTimeoutHandler` / `IOInterruptEventSource`
  atau `thread_call` (lihat `itl80211/compat.cpp` — sudah ada shim-nya);
* `mtx_/rw_` → `IOLock` / `IOService::serializeProperties` (lihat `docs/porting-notes.md`);
* `bpf_mtap` (2×) → tulis radiotap ke `mbuf` langsung (net80211 itlwm sudah pakai `mbuf` API
  `IOBSD` dari `compat.cpp`).

### 3.3 LAYER 3 — net80211

Titik integrasi utama: `ieee80211_ioctl()` dengan sub-command `IEEE80211_IOCTL_*`.
`urtwn` mendaftarkan diri sebagai `ieee80211_com`; semua perintah scan/join/key
melewati kontrak ini. Tidak ada perubahan yang perlu dibuat di net80211 — cukup adaptasi
nama fungsi (OpenBSD vs versi itlwm) yang sedikit berbeda.

### 3.4 LAYER 4 — frontend IO80211

`AirportItlwm` mendaftarkan `IO80211Controller` ke stack IO80211 Apple, lalu membuat
`IO80211Interface` untuk uplink ke CoreWLAN. Yang perlu diadaptasi:
* `IOPCIEDeviceWrapper` → ganti dengan **USB device wrapper** (`IOUSBHostInterface`).
* Panggilan ke Intel `iwl_*`/`iwlm_*` → ganti ke `urtwn_*`/`rtwn_*`.
* `AirportItlwmInterface` / `AirportItlwmSkywalkInterface` → sebagian besar bisa dipakai
  apa adanya karena kontrak IO80211 sama.

---

## 4. Alur data utama

**Scan**
```
CoreWLAN → IO80211Interface::scan → IO80211Controller → net80211 ieee80211_ioctl(SCAN)
          → urtwn: set channel 1-13, set NAV, kirim probe request via bulk OUT
          → chip RX beacon/probe response → urtwn_collect_rssi → ieee80211_input()
          → net80211 builds scan results → IO80211Interface::appendScanResult
          → CoreWLAN menampilkan daftar SSID di menu Wi-Fi
```

**Join WPA2**
```
CoreWLAN (airportd) → IO80211Interface::setSSID(+RSN IE) → net80211 state machine
   AUTH → Assoc → EAPOL 4-way di sisi Apple (WCL) → driver hanya Titelkan frame EAPOL
   ke/dari chip (EAPOL data path transparan lewat 8188EUS)
   → net80211 ieee80211_ioctl(KEY) → urtwn_set_key → rtwn_set_key → vendor cmd
```

**Data TX/RX**
```
App → Ethernet → IO80211Interface → net80211 ieee80211_output → urtwn TX ring
   → bulk OUT → chip → AP
chip → bulk IN → urtwn RX ring → radiotap → ieee80211_input → net80211 → IO80211Interface
```

---

## 5. Struktur direktori source

```
src/
├── airport/            # Layer 4 — frontend IO80211 (C++)
│   ├── AirportItlwmController.cpp     # adaptasi dari AirportItlwm.cpp
│   ├── AirportItlwmInterface.cpp      # adaptasi
│   ├── AirportItlwmSkywalkInterface.cpp
│   ├── AirportSTAIOCTL.cpp
│   ├── USBBackend.hpp                 # ★ baru: pembungkus IOUSBHostInterface
│   └── include/Airport/*              # header privat Apple (dari itlwm)
├── net80211/           # Layer 3 — dari itl80211 (C/C++)
├── urtwn/              # Layer 2 — dari OpenBSD (C)
│   ├── if_urtwn.c
│   ├── rtwn.c
│   ├── rtwnvar.h
│   └── r92creg.h
├── usb/                # Layer 1 — shim (C/C++)
│   ├── usb_compat.h/.cpp               # usbd_* → IOUSBHostPipe
│   └── bsd_compat.h/.cpp              # mbuf/mutex/timeout BSD→kernel
└── m0-hello/           # Milestone 0 — kext minimal (sudah bisa di-build)
```

---

## 6. Yang sengaja TIDAK diimplementasikan

| Fitur | Alasan |
|---|---|
| AWDL / AirDrop / Handoff | `urtwn` tidak mengimplementasikan AWDL; butuh frame management 802.11 khusus yang tidak ada di driver open-source ini. Project AirportItlwm (Intel) juga tidak mendukungnya |
| 5 GHz / 802.11ac/ax | Chip 8188EUS = 2,4 GHz 802.11n saja |
| 802.1X (EAP) | Butuh supplicant; AirportItlwm tidak membawa pun (pakai WCL Apple). RoadmapLUAR untuk versi corporate |
| Monitor mode | OpenBSD punya mode `IEEE80211_MONITOR` di `urtwn`, tapi memportirnya ke IO80211 tidak sebanding dengan usaha-nya |
| Channel > 13 |_unlock regulatory; butuh `ieee80211_regdomain` proper + country code |

---

## 7. Risiko arsitektural & mitigasi

| Risiko | Dampak | Mitigasi |
|---|---|---|
| IO80211 KPI berubah tiap rilis macOS | Driver mati setelah update | Pin `MaxKernel`; jaga build per versi (seperti AirportItlwm: Monterey/Sonoma/Sequoia) |
| `net80211` itlwm sudah dimodifikasi untuk Intel | Potensi konflik dengan `urtwn` | `urtwn` pakai subset API net80211 yang stabil; audit per fungsi |
| USB power management (suspend/resume) | Wi-Fi mati setelah tidur | `setPowerState`, muat ulang firmware saat wake (bagianXRUS paling sering bug) |
| Throughput rendah tanpa agregasi | ~30–60 Mbps nyata (bukan 150) | Implementasi A-MPDU belum ada; OpenBSD urtwn belum mendukungnya |
| ESP (dulu 16,8 MB) | Build mungkin tidak muat | ✅ SELESAI: ESP_now 512 MB (`EFI-BIG`, sisa 477 MB) — plenty |
