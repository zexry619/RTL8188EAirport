# Katalog Referensi

Semua tautan sudah diverifikasi (HTTP 200) pada **21 Oktober 2026**. Kolom "Pakai untuk"
menunjukkan peran file dalam proyek ini.

---

## A. Driver hardware (OpenBSD — basis utama)

| # | Sumber | Lisensi | Pakai untuk |
|---|---|---|---|
| A1 | [`openbsd/src` → `sys/dev/usb/if_urtwn.c`](https://github.com/openbsd/src/blob/master/sys/dev/usb/if_urtwn.c) (2.603 baris) | ISC | **Inti driver kita.** USB attach, firmware upload, EFUSE, RF, TX/RX ring, scan/join |
| A2 | [`openbsd/src` → `sys/dev/ic/rtwn.c`](https://github.com/openbsd/src/blob/master/sys/dev/ic/rtwn.c) (3.424 baris) | ISC | Kode bersama Realtek: `rtwn_fw_cmd`, `rtwn_efuse_read`, `rtwn_read_chipid`, AMRR rate control, key mgmt |
| A3 | [`sys/dev/ic/rtwnvar.h`](https://github.com/openbsd/src/blob/master/sys/dev/ic/rtwnvar.h) | ISC | Struktur data bersama (`struct rtwn_softc`) |
| A4 | [`sys/dev/ic/r92creg.h`](https://github.com/openbsd/src/blob/master/sys/dev/ic/r92creg.h) (127 KB) | ISC | Register map Realtek 87xx/92xx: DMAC, MAC, EFUSE, RF |
| A5 | [`sys/dev/usb/usbdevs`](https://github.com/openbsd/src/blob/master/sys/dev/usb/usbdevs) | ISC | Tabel USB ID. **Baris 4551:** `product TPLINK RTL8188EUS 0x010c` |
| A6 | [`man.openbsd.org/urtwn`](https://man.openbsd.org/urtwn) | — | Dokumentasi perilaku driver, chip yang didukung |
| A7 | [OpenBSD CVS — if_urtwn.c](https://cvsweb.openbsd.org/src/sys/dev/usb/if_urtwn.c) | ISC | Riwayat revisi (untuk melihat perubahan antar versi) |
| A8 | [FreeBSD `if_urtwn.c`](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/usb/wlan/if_urtwn.c) | BSD-2 | Cross-checkporting (FreeBSD net80211 berbeda, tapi logika USB sama) |
| A9 | [FreeBSD `rtwn`](https://github.com/freebsd/freebsd-src/tree/main/sys/dev/rtwn) | BSD-2 | Cross-check |

> **Bukti kunci:** di `if_urtwn.c` baris 332 ada entri
> `URTWN_DEV_8188EU(TPLINK, RTL8188EUS)` — device kita **tidak perlu patch tabel ID**.

---

## B. Stack 802.11 (net80211 OpenBSD — sudah ada versinya untuk macOS)

| # | Sumber | Lisensi | Pakai untuk |
|---|---|---|---|
| B1 | [`laobamac/itlwm` → `itl80211/openbsd/net80211/`](https://github.com/laobamac/itlwm/tree/main/itl80211/openbsd/net80211) (34 file) | ISC (asal OpenBSD) | **Pakai langsung.** Port net80211 ke Darwin + shim kompatibilitas BSD→kernel |
| B2 | [`itl80211/compat.cpp`, `compat.h`](https://github.com/laobamac/itlwm/tree/main/itl80211) | GPL-2.0 |Contoh shim: `malloc`, `timeout`, `mtx`, `mbuf`, `arc4random`, dll. |
| B3 | [`openbsd/src` → `sys/net80211/`](https://github.com/openbsd/src/tree/master/sys/net80211) | ISC | Sumber asli bila perlu versi lebih baru |
| B4 | [`itl80211/openbsd/net80211/ieee80211_ioctl.c` + `ieee80211_ioctl.h`](https://github.com/laobamac/itlwm/tree/main/itl80211/openbsd/net80211) | ISC | Kontrak ioctl/net80211 ↔ driver. **`urtwn` memanggil `ieee80211_ioctl()`** — titik integrasi utama |

---

## C. Frontend IO80211 (bridge ke CoreWLAN)

| # | Sumber | Lisensi | Pakai untuk |
|---|---|---|---|
| C1 | [`laobamac/itlwm` → `AirportItlwm/`](https://github.com/laobamac/itlwm/tree/main/AirportItlwm) | GPL-2.0 | **Pola frontend kita**: `AirportItlwm.cpp`, `AirportItlwmInterface.cpp`, `AirportItlwmSkywalkInterface.cpp`, `AirportSTAIOCTL.cpp`, `AirportVirtualIOCTL.cpp`, `AirportAWDL.cpp`, `IOPCIEDeviceWrapper.cpp` |
| C2 | [`include/Airport/*`](https://github.com/laobamac/itlwm/tree/main/include/Airport) (28 header privat Apple) | Apple privat | Kontrak IO80211: `IO80211Controller.h`, `IO80211Interface.h`, `IO80211SkywalkInterface.h`, `apple80211_var.h`, `apple80211_ioctl.h` |
| C3 | [`OpenIntelWireless/itlwm`](https://github.com/OpenIntelWireless/itlwm) (master, commit terakhir 2024-06-09) | GPL-2.0 | Upstream resmi; dipakai sebagai sumber net80211 yang lebih bersih |
| C4 | [`JoMei9019-real/AirPort-RTW`](https://github.com/JoMei9019-real/AirPort-RTW) | GPL-2.0 | **Pola frontend + USB**: `src/kext/RTW88USBDevice.cpp` sudah memakai `IOUSBHostDevice`/`IOUSBHostPipe` + shim kompatibilitas Linux |
| C5 | [`AirPort-RTW` → `src/compat/`](https://github.com/JoMei9019-real/AirPort-RTW/tree/main/src/compat) | GPL-2.0 | Contoh shim `rtw88_compat.h` untuk mencocokkan API Linux ke IOKit |
| C6 | [`AirportItlwm` → `AirportItlwmEthernetInterface.cpp`](https://github.com/laobamac/itlwm/blob/main/AirportItlwm/AirportItlwmEthernetInterface.cpp) | GPL-2.0 | Jalur bring-up lebih mudah: mode `IOEthernet` dulu, baru mode `IO80211` |
| C7 | [itlwm Wiki](https://github.com/OpenIntelWireless/itlwm/wiki) | — | Cara build, kebutuhan SDK, catatan kompatibilitas |

---

## D. Header & sumber kernel macOS

| # | Sumber | Pakai untuk |
|---|---|---|
| D1 | [`acidanthera/MacKernelSDK`](https://github.com/acidanthera/MacKernelSDK) (20 MB, headers Apple 10.4–15.x) | **Wajib** untuk build kext. `Headers/` + `Library/x86_64/libkmod.a` |
| D2 | `xnu` → [`apple-oss-distributions/xnu`](https://github.com/apple-oss-distributions/xnu) | Referensi internal XNU (IOKit, KPI, `IOKit/IOUSBHost*`) |
| D3 | [`opensource.apple.com/source/IOKit`](https://opensource.apple.com/source/IOKit/) | Implementasi `IOUSBHostDevice`, `IOUSBHostPipe`, `IOUSBHostInterface` |
| D4 | [`opensource.apple.com/source/IOUSBHostFamily`](https://opensource.apple.com/source/IOUSBHostFamily/) | Detail perilaku USB host stack modern (bukan legacy `IOUSBInterface`) |
| D5 | [`dortania/OpenCore-Legacy-Patcher` → `payloads/Kexts/Wifi`](https://github.com/dortania/OpenCore-Legacy-Patcher/tree/main/payloads/Kexts/Wifi) | Header `IO80211FamilyLegacy` — hanya perlu kalau targeting stack legacy (J Ventura). Untuk Sequoia: pakai stack native |
| D6 | SDK lokal: `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Headers` | Fallback header IOKit (tapi **harus** `-DKERNEL=1 -DKERNEL_PRIVATE=1`, lihat `docs/porting-notes.md`) |
| D7 | `/System/Library/Kernels/BootKernelExtensions.kc` | Sumber kode IO80211 yang sebenarnya (yang di `/System/Library/Extensions/IO80211Family.kext` cuma stub 12 KB) |

---

## E. USB &Reverse Engineering perangkat

| # | Sumber | Pakai untuk |
|---|---|---|
| E1 | [Linux `rtl8xxxu`](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtl8xxxu) | Cross-check urutan init, EFUSE offset, vendor command (GPL) |
| E2 | [Linux `rtl8188eu` (staging)](https://github.com/torvalds/linux/tree/master/drivers/staging/rtl8188eu) | Cross-check 8188EU family (GPL) |
| E3 | [Linux `rtlwifi` (8188EE)](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtlwifi) | Source driver 8188EE original (GPL) |
| E4 | [Linux `rtw88` (USB)](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtw88) | Pola driver USB modern untuk chipset Realtek (GPL) |
| E5 | [USB 2.0 specification](https://www.usb.org/documents) | Deskriptor, endpoint, URB |
| E6 | [Wireshark USB capture](https://www.wireshark.org/docs/wsdg_html_chunked/) | Analisis lalu lintas USB (butuh `usbmon` di Linux) |
| E7 | [OpenBSD `usbdevs`](https://github.com/openbsd/src/blob/master/sys/dev/usb/usbdevs) | Tabel ID chip Realtek (0x010c = 8188EUS) |

---

## F. Firmware

| # | Sumber | Pakai untuk |
|---|---|---|
| F1 | [`rtl8188eufw.bin` (linux-firmware, kernel.org)](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtlwifi/rtl8188eufw.bin) | Firmware upload ke 8188EUS. **15.262 byte**, sha256 `2ff74315287529dec2e50eb57d6e0c97d2116f28ae166773ccdf93b6360000c4` |
| F2 | [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) | Sumber firmware lengkap + lisensi (`LICENCE.rtlwifi_firmware`) |
| F3 | [OpenBSD port `urtwn-firmware`](https://cvsweb.openbsd.org/ports/sysutils/urtwn-firmware/) | Cara OpenBSDamol firmware untuk `urtwn` |
| F4 | [Realtek USB WiFi driver Linux (vendor)](https://github.com/Realtek-OpenSource/realtek_wifi) | Cross-check vendor command & firmware |

> Firmware **tidak** boleh disalin dari paket Realtek untuk macOS (`StatusBarApp`).
> Ambil dari linux-firmware (F1).

---

## G. Toolchain & debugging

| # | Tool | Keterangan |
|---|---|---|
| G1 | `clang` (Command Line Tools, sudah ada di mesin ini: 17.0.0) | Build kext via Makefile |
| G2 | `kmutil` / `kextutil` / `codesign` ( sudah ada di `/usr/bin`) | Validasi & load kext |
| G3 | `ioreg -r -c <Class>` | Melihat driver bind ke device apa |
| G4 | `log stream --predicate '…'`, `log show` | Log kernel (`IOLog`, `OSReport`) |
| G5 | `lldb` + KDP | Debug kernel jarak jauh (butuh 2 mesin / ethernet) |
| G6 | [Hopper Disassembler](https://www.hoppersdisassembler.com/) (komersial) / [Ghidra](https://ghidra-sre.org/) (gratis) | RE binary vendor sebagai *oracle* |
| G7 | `otool -tvV`, `nm -nm`, `strings`, `class-dump` | RE statis pada `RtWlanU` & `StatusBarApp` |
| G8 | [USB Prober](https://www.sulake.com/software/usbprober/) / Apple USB Prober (Xcode) | Melihat transfer USB & endpoint |
| G9 | Linux VM + `usbmon` + Wireshark | Capture lalu lintas USB (macOS tidak punya usbmon) |

---

## H. Dokumentasi Apple (arsip)

| # | Dokumen | Pakai untuk |
|---|---|---|
| H1 | [Kernel Programming Guide](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/) | Struktur kext, threading, I/O, power management |
| H2 | [KEXT Concepts](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KEXTConcept/KEXTConcept.html) | Anatomi Info.plist, personality, matching, OSBundleLibraries |
| H3 | [IOKit Concepts](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/IOKitConceptual/) | Publish/attach, `IOService::match`, `IOUSBHostInterface` |
| H4 | [Kernel Debugging (KDP)](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/KernelDebug/KernelDebug.html) | Remote debug kernel, watchpoint, register |
| H5 | [Writing a KEXT](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/Programming/WritingKexts/WritingKexts.html) | Pola kode kext, `start()`/`stop()`, matching |
| H6 | [Energy & power management](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/EnergyManagement/) | `setPowerState`, wake, USB suspend/resume |

---

## I. Spesifikasi chip & adapter

| # | Sumber | Isi |
|---|---|---|
| I1 | [Realtek RTL8188EUS datasheet (RTL8188EUS_V3.x)](https://www.realtek.com/DownloadList?cate_id=816) | Register map, block diagram, 2,4 GHz 1×1 HT40 |
| I2 | TP-Link TL-WN722N v3 (USB dongle) | Adapter produk ini memakai RTL8188EUS |
| I3 | `ioreg` di mesin ini | USB ID 9047/268, `bcdDevice`, kecepatan bus |

---

## J. Project sejenis (pola & pelajaran)

| # | Project | Pelajaran |
|---|---|---|
| J1 | [itlwm / AirportItlwm](https://github.com/OpenIntelWireless/itlwm) | **Pola utama.** Frontend IO80211 + net80211 port + firmware multi-chip |
| J2 | [AirPort-RTW](https://github.com/JoMei9019-real/AirPort-RTW) | Frontend Realtek + **pola shim USB** (`RTW88USBDevice.cpp`) |
| J3 | [RTL8188EEWireless](https://github.com/rehmansaeed06-byte/RTL8188EEWireless) | Port driver Linux `rtlwifi` ke macOS + compat layer. **Status: paused karena bug RX stall** — penting: 802.11 driver yang stabil itu sulit |
| J4 | [HoRNDIS](https://github.com/taylorbrand/HoRNDIS) | Contoh kext USB networking di macOS (bukan Wi-Fi, tapi pola USB pipe) |
| J5 | [AppleHDA** / open kext lain di Acidanthera](https://github.com/acidanthera) | Standar kualitas: build script, Info.plist per-OS, style |

---

## K. Referensi lokal di mesin ini

| Path | Isi |
|---|---|
| `/Library/Extensions/RtWlanU.kext` | Driver vendor Realtek 1830.32.b27 (2020-11-19). Binary tertutup. Hanya untuk observasi |
| `/Library/Application Support/WLAN/StatusBarApp.app` | Kontrol plane vendor: OID, EAPOL, profile. Native macOS **tidak** punya ini |
| `/Library/Application Support/WLAN/com.realtek.utility.wifi/RtWlanHelper` | Daemon pendukung vendor |
| `/Library/Application Support/WLAN/StatusBarApp.app/Contents/MacOS/wpa_supplicant` | Supplicant bawaan vendor (untuk 802.1X) |
| `/System/Library/Extensions/IO80211Family.kext` | **Stub 12 KB** — kode asli ada di `BootKernelExtensions.kc` |
| `/System/Library/Extensions/AppleBCMWLANBusInterfacePCIeMac.kext` | Driver Broadcom native — contoh "bentuk" driver IO80211 yang benar |
| ESP OpenCore | `EFI-BIG` = `/dev/disk0s3`, 512 MB (sisa 477 MB), berisi `EFI/OC` lengkap (OpenCore 1.0.8 RELEASE, 12 kext, OpenShell). Volume UUID `DDA2169E-423F-3A6D-8C3A-F8371F48D184`. Driver custom ±2–4 MB → **muat tanpa masalah** |

---

## L. Bacaan lanjutan (urutan belajar)

1. `man.openbsd.org/urtwn` → pahami perilaku driver.
2. `if_urtwn.c` + `rtwn.c` → pahami urutan: attach → firmware → EFUSE → RF → scan.
3. `itl80211/openbsd/net80211/ieee80211_ioctl.c` → pahami kontrak driver↔stack.
4. `AirportItlwm/AirportItlwm.cpp` + `AirportItlwmInterface.cpp` → pahami kontrak driver↔CoreWLAN.
5. `AirPort-RTW/src/kext/RTW88USBDevice.cpp` → pahami transport USB di IOKit.
6. `docs/porting-notes.md` → daftar API yang harus di-replace.

## M. Tool RE (terpasang di mesin ini)

| # | Tool | Lokasi | Catatan |
|---|---|---|---|
| M0 | [REA](https://github.com/morluto/rea) (`rea-agents` 6.7.0) | `~/.local/node/bin/rea` | MCP server (207 tool) + CLI; terdaftar di `~/.config/opencode/opencode.json` → `mcp.rea` |
| M1 | [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | `~/.local/ghidra/ghidra_12.1.4_PUBLIC` | `GHIDRA_INSTALL_DIR`; decompiler `mac_x86_64` disalin dari 11.4.3 |
| M2 | Ghidra 11.4.3 | `~/.local/ghidra/ghidra_11.4.3_PUBLIC` | satu-satunya versi dengan native decompiler macOS x86_64 |
| M3 | JDK Temurin 21.0.12.1 | `~/.local/jdk` | `JAVA_HOME` |
| M4 | Node.js 24.18.0 | `~/.local/node` | syarat REA (`npx rea-agents`) |
| M5 | RE.tools (katalog) | https://rea.tools/ | daftar tool RE terkurasi |
| M6 | Rizin | (tidak dipakai) | paket macOS hanya arm64; mesin ini Intel → `EBADARCH` |

## N. User-space USB (untuk validasi aman)

| # | Sumber | Keterangan |
|---|---|---|
| M1 | `IOKit/usb/IOUSBLib.h` (SDK user space) | `IOUSBDeviceInterface` → `DeviceRequest()`, `USBDeviceOpen/Close` — API USB legacy dari user space |
| M2 | `IOKit/usb/USB.h` | `IOUSBDevRequest` (bmRequestType/bRequest/wValue/wIndex/wLength/pData/wLenDone) |
| M3 | `tools/rtwn_probe.c` (proyek ini) | Implementasi probe: baca register 8188E via vendor request, tanpa kernel |
