# M4 — Integrasi IO80211/AirPort native macOS (22 Okt 2026)

Rujukan ini dikumpulkan dari **source driver nyata** yang sudah terbukti bekerja
di macOS Sequoia (15.x = target kita).

---

## 1. Driver referensi yang ditemukan

| Driver | Repo | Chip | macOS | Status |
|---|---|---|---|---|
| **AirPort_RTW88** 2.0.1 | `xnoah222/Airport_RTW88` | RTL88xx **PCIe** (8822BE/8822CE/8821CE) | Ventura 13.7.7 → Tahoe 26 | ✅ **native AirPort** |
| **AirPort_RTW89** | `FranciumOS-org/Airport_RTW89` | RTL89xx PCIe (8852AE/BE/CE, 8922AE…) | Ventura 13 → Tahoe 26 | ✅ native AirPort |
| **Realtek88LegacyAirport** 1.0.1 | `xnoah222/Realtek-AirPort-Family` | RTL88xx PCIe | Mojave–Big Sur (HS80211Family) | ✅ legacy AirPort |
| **AirportItlwm** | `OpenIntelWireless/itlwm` | Intel | Sonoma/Sequoia | ✅ native AirPort (udah di-vendor di proyek) |

**Sudah diunduh ke proyek ini:**
```
upstream/Airport_RTW88/AirPort_RTW88/
├── rtw88-stable/      ← Linux rtw88 (GPL-2.0), termasuk USB transport
└── Feixiao/           ← driver macOS native-AirPort (GPL-2.0 / BSD-3)
    ├── src/kext/      ← AirPortRTW88Kext.cpp, AirportRTW88Interface.hpp,
    │                     RTW88IEEE80211.cpp, RTW88HwOps.cpp, AWDL, dll
    ├── src/compat/    ← shim Linux→macOS ( termasuk linux/usb.h)
    └── MacKernelSDK/  ← header kernel macOS
```
Total **515.143 baris** C/H — referensi M4 terlengkap yang bisa kita dapat.

> ⚠️ `Feixiao` **PCIe only** — USB sengaja dikecualikan dari build-nya.
> Tapi **integrasi IO80211-nya persis yang kita butuh**, dan `rtw88-stable`
> memuat kode transport USB.

---

## 2. Struktur kext yang dipakai Airport_RTW88

Dari `Feixiao/AirPort_RTW88.kext/Contents/Info.plist`:

```xml
<key>CFBundleIdentifier</key>  <string>com.rtw88.airport</string>
<key>LSMinimumSystemVersion</key> <string>13.0</string>
<key>OSBundleLibraries</key>
<dict>
    <key>com.apple.iokit.IO80211FamilyLegacy</key> <string>12.0</string>
    <key>com.apple.iokit.IONetworkingFamily</key>  <string>3.4</string>
    <key>com.apple.iokit.IOPCIFamily</key>          <string>2.9</string>
    <key>com.apple.iokit.IOSkywalkFamily</key>       <string>1.0</string>
    <key>com.apple.kpi.bsd|iokit|libkern|mach|unsupported</key> <string>20.0.0</string>
</dict>
<key>IOKitPersonalities</key>
<dict>
  <key>AirPort_RTW88_8822BE</key>
  <dict>
    <key>CFBundleIdentifier</key> <string>com.rtw88.airport</string>
    <key>IOClass</key>            <string>AirportRTW88</string>
    <key>IOMatchCategory</key>    <string>IODefaultMatchCategory</string>
    <key>IOPCIMatch</key>         <string>0xB82210EC</string>   ← device|vendor
    <key>IOProbeScore</key>       <integer>5000</integer>
    <key>IOProviderClass</key>    <string>IOPCIDevice</string>
  </dict>
  ...
</dict>
```

**Untuk USB (chip kita)**, personality-nya jadi:

```xml
<key>RTL8188E_USB</key>
<dict>
  <key>CFBundleIdentifier</key> <string>io.github.zekri.RTL8188EAirport</string>
  <key>IOClass</key>            <string>IOPCIEDeviceWrapper</string>
  <key>IOProviderClass</key>    <string>IOUSBHostInterface</string>
  <key>idVendor</key>           <integer>9047</integer>   <!-- 0x2357 -->
  <key>idProduct</key>          <integer>268</integer>    <!-- 0x010c -->
  <key>IOProbeScore</key>       <integer>10000</integer>
</dict>
```

> Nama kelas wrapper tetap `IOPCIEDeviceWrapper` supaya frontend
> (`OSDynamicCast`) tidak perlu diubah besar-besaran. Di jalur USB
> `pciNub = nullptr`; wrapper menyimpan `IOUSBHostInterface`.

---

## 3. ABI macOS 15.2+: V2 / Skywalk (WAJIB)

`IO80211Controller` lama (5 pure virtual) **tidak cukup** untuk macOS 15.8.1.
Sequoia memakai ABI **V2/Skywalk**:

```
AIRPORT_WCL
AIRPORT_VTD          (boleh dilepas kalau tidak memakai ItlDma)
__IO80211_TARGET=__MAC_15_2
AIRPORT
__PRIVATE_SPI__
IO80211FAMILY_V2
```

Sumber: target `AirportItlwm-Sequoia` di
`upstream/itlwm/itlwm.xcodeproj/project.pbxproj`.

**Trik build penting:** Xcode menyetel `GCC_INPUT_FILETYPE = sourcecode.cpp.cpp`
untuk target Sequoia — artinya seluruh file net80211 `.c` **dikompilasi sebagai
C++**. Tanpa ini, header IOKit C++ (`OSMetaClass.h`) gagal di-parse sebagai C.
Build kita meniru persis: `.c` net80211 → `clang++ -x c++`.

**Daftar sumber target Sequoia** (75 file) sudah diekstrak; untuk M4 kita
memakai subset:

| Kelompok | File |
|---|---|
| net80211 | `ieee80211*.c` (18), `_string.c`, `timeout.c`, `CTimeout.cpp` |
| crypto | `aes/hmac/sha1/sha2/rijndael/ccmp/michael/...` (22) |
| support | `compat.cpp`, `_mbuf.cpp`, `_task.cpp`, `_ifq.cpp`, `zutil.c`, `arp.c` |
| frontend | `AirportItlwm.cpp`, `AirportItlwmSkywalkInterface.cpp`, `AirportItlwmEthernetInterface.cpp`, `WCL.cpp` |
| HAL | `ItlHalService.cpp`, `ItlRtl8188E.cpp` |
| chip | `rtwn_seq.c`, `rtwn_seq_efuse.c`, `rtwn_init_8188e.c`, `rtwn_usb.c`, `usb_compat.cpp`, firmware |

Intel HAL (`ItlIwm/Iwx/Iwn`), `FwBinary.cpp`, dan `ItlDma.cpp` **tidak**
dikompilasi.

---

## 4. Arsitektur M4 yang dibangun

```
IOUSBHostInterface (2357:010c)
        │  personality RTL8188EUSWrapper
        ▼
IOPCIEDeviceWrapper  (src/airport/IOPCIEDeviceWrapper.cpp)
        │  membuat ItlRtl8188E, menyimpan IOUSBHostInterface
        │  registerService()
        ▼
AirportItlwm  (src/airport/AirportItlwm.cpp — salinan V2)
        │  IO80211Controller / Skywalk
        │  fHalService->attach(nullptr)
        ▼
ItlRtl8188E  (src/driver/ItlRtl8188E.cpp)
        │  net80211 (ic_newstate, if_start, RX)
        ▼
rtwn_seq / rtwn_init_8188e / rtwn_usb  (kode M1/M2 yang sama)
        ▼
USB bulk OUT / IN
```

### 4.1 Frontend yang dipatch (minimal)

Hanya tiga hal di `src/airport/AirportItlwm.cpp`:

1. `probe()` — tidak lagi mewajibkan `pciNub` (USB tidak punya).
2. `start()` — blok PCI (`setBusMasterEnable`, `initPCIPowerManagment`, dsb.)
   dibungkus `if (pciNub) { ... }`.
3. `super::stop(provider)` pada jalur error (bukan `pciNub`).

`fHalService->attach(pciNub)` tetap dipanggil dengan `nullptr`; HAL kita
mengabaikan argumen itu dan memakai `fInterface` yang disimpan saat `prepare()`.

### 4.2 HAL `ItlRtl8188E`

| Tahap | Isi |
|---|---|
| `prepare()` | simpan `IOUSBHostInterface` + device (retain) |
| `attach()` | `RtwnUsb::attach` → power-on → LLT/DMA → firmware 15.262 B → EFUSE (offload, fallback direct) → MAC/BB/RF/core config → `bulkOpen` + 8 RX |
| net80211 | `ifp->if_softc=this`, `if_start`, `if_ioctl=ieee80211_ioctl`, `ieee80211_ifattach`, `ieee80211_media_init` |
| scan | `ic_newstate(SCAN)` → set kanal via `rtwn_set_chan_8188e` → timer 120 ms → `ieee80211_next_scan` |
| TX mgmt | `ic_mgtq` → descriptor 32 B (QSEL=MGNT, RAID 11B, HWSEQ, CCK1) → bulk |
| TX data | `if_snd` → `ieee80211_encap` → kripto software bila protected → descriptor (DRVRATE, CCK 1M, HWSEQ/QoS seq) → bulk |
| RX | parse 24 B desc + `infosz` PHY (kandidat offset +0/+2) → mbuf → `ieee80211_inputm` |
| detach | timeout_free → `ieee80211_ifdetach` → radio off → bulk close → USB detach |

**Kripto**: CCMP/TKIP/WEP ditangani **software** oleh net80211
(`ieee80211_encrypt` / `ieee80211_decrypt`). Tidak ada offload hardware — ini
membuat WPA2 mungkin tanpa menyentuh register security chip.

---

## 5. Status build (22 Okt 2026)

| Cek | Hasil |
|---|---|
| `scripts/build-m4.sh` | ✅ **rc=0**, `RTL8188EAirport.kext` (x86_64) |
| Frontend V2 compile | ✅ 0 error (4 file) |
| net80211 sebagai C++ | ✅ 0 error |
| Link kext | ✅ hanya undefined simbol kernel/IO80211 (normal) |
| `kextutil -t` | ✅ semua simbol **bind**; berhenti hanya di *"not approved to load"* (kebijakan kext, bukan masalah simbol) |
| Uji hardware | ⏳ belum — dongle belum tercolok |

Artinya: **tidak ada simbol yang hilang** antara kext kita dengan
IO80211Family/IOSkywalkFamily/IOUSBHostFamily di macOS 15.8.1. Ini menghapus
risiko terbesar "kext tidak bisa load" sebelum reboot.

---

## 6. Yang BELUM sempurna (jujur)

1. **Asosiasi/WPA2 belum diuji hardware.** Jalur scan sudah lengkap; jalur
   RUN masih minimal:
   - belum menulis BSSID/EDCA/RX filter khusus setelah join (RCR saat ini
     menerima semua — cukup untuk uji awal),
   - laju TX dipaku **1 Mb/s CCK** (belum ada rate control/AMRR),
   - belum ada AMPDU/HT, jadi throughput rendah.
2. **RSSI** sudah dihitung dari PHY status (`rtwn_r88e_get_rssi`), tetapi
   belum dikalibrasi terhadap hardware nyata.
3. **Manajemen daya** mengandalkan timer net80211; belum ada kalibrasi suhu.
4. **`AirportItlwm::stop`** memanggil `fHalService->detach(nullptr)`; aman
   karena HAL mengabaikan argumen.
5. **Kext harus di-inject lewat OpenCore** (BootKC); `kextutil` biasa ditolak
   kebijakan "not approved". Ini sama seperti M1.

---

## 7. Rencana uji satu reboot

```bash
# 0. PASTIKAN DONGLE 2357:010c TERC0LOK (tanpa dongle, kext tidak match)
system_profiler SPUSBDataType | grep -A4 "802.11n NIC"

# 1. build + pasang (M1 otomatis dibuang dari ESP)
ALLOW_BOOT_INJECTION=1 sudo ./scripts/install-m4-esp.sh

# 2. reboot

# 3. setelah login — cek driver
kextstat | grep RTL8188EAirport
ioreg -r -c ItlRtl8188E -w0 | head -30
networksetup -listallhardwareports | grep -A2 -i wi-fi

# 4. Wi-Fi menu → harus muncul daftar SSID (scan)
#    pilih ZEKRI.NET → uji asosiasi/DHCP

# 5. kalau gagal total / panic → cabut dongle, lalu
sudo ./scripts/restore-test-esp.sh
```

**Rollback**: `scripts/restore-test-esp.sh` mengembalikan `config.plist` dan
seluruh EFI dari backup; M1 bisa dipasang lagi dengan
`scripts/install-test-esp.sh`.

---

## 8. Keterkaitan dengan rencana awal

Rencana awal kita: "port net80211 dari itlwm + AirportItlwm frontend".
**Tetap valid** — dan sekarang sudah dieksekusi sampai kext utuh.

| Yang mau | Template terbaik |
|---|---|
| Integrasi IO80211 modern | `upstream/itlwm/AirportItlwm` (dipakai) |
| Integrasi IO80211 + hw-ops Realtek | `upstream/Airport_RTW88/.../Feixiao/src/kext/` |
| Lapisan IEEE 802.11 lengkap (scan/join/keys/RSN) | net80211 itlwm (dipakai) |
| Transport USB | `src/usb/usb_compat.{h,cpp}` (kita sudah tulis!) |
| HAL chip 8188E | `src/urtwn/rtwn_seq.c` + `rtwn_init_8188e.c` (sudah terbukti M2) |

---

## 9. Lisensi

| Komponen | Lisensi | Cocok dengan GPL-2.0 kita? |
|---|---|---|
| Linux `rtw88` | GPL-2.0 | ✅ |
| `Feixiao` (AirPort_RTW88) | GPL-2.0 OR BSD-3-Clause | ✅ |
| OpenBSD `urtwn` (kita) | ISC | ✅ (boleh di dalam GPL) |
| `itlwm` / `AirportItlwm` | GPL-2.0 | ✅ |
| `MacKernelSDK` | Apple (headers) | ✅ (headers saja) |

Lihat `docs/legal.md`.


---

## 10. Audit pra-reboot (22 Okt 2026) — bug kritis yang ditemukan & diperbaiki

Audit dilakukan **tanpa hardware** setelah build sukses. Tujuannya: memastikan
satu-satunya reboot uji tidak berakhir panic karena hal sepele.

| # | Temuan | Dampak kalau tidak diperbaiki | Perbaikan |
|---|---|---|---|
| 1 | `ifp->if_watchdog` NULL | `watchdogAction` memanggil pointer NULL **1 detik setelah Wi-Fi ON** → panic | `watchdogCb` dipasang di `attach` |
| 2 | Tabel `ic_channels[1..13]` kosong | `ieee80211_channel_init` tidak menandai satu kanal pun → scan tidak pernah mengunjungi kanal, menu kosong | isi freq/flags 2.4 GHz **sebelum** `ieee80211_ifattach` |
| 3 | RX diantre sebelum net80211 attach | frame datang saat `ic_bss == NULL` → NULL deref di `ieee80211_find_rxnode` | RX baru dimulai setelah ifattach + event source siap |
| 4 | RX diproses di workloop USB | `ieee80211_inputm` (alokasi/sleep) di workloop USB; `runAction` blocking bisa deadlock dengan control transfer | callback USB hanya **menyalin** ke ring 64 slot, timer 0 ms memproses di workloop IO80211 |
| 5 | `free()` tanpa `ifdetach` saat start gagal | timer net80211 (slowtimo) menembak `ifp` yang sudah dibebaskan → panic tertunda | `fIfAttached` + `ieee80211_ifdetach` di `free()` |
| 6 | `sendFrame` error-path | mbuf + referensi node bocor | semua jalur melepas `ni` dan `m` |
| 7 | Offset RX +2 (heuristik M2) | stride paket bergeser → paket berikutnya dalam satu transfer dibuang | pakai format canonical OpenBSD: `24 + infosz`, stride `24 + infosz + pktlen` (align 4) |
| 8 | RSSI stub -55 | menu menampilkan sinyal palsu | parse `r88e_rx_phystat` (CCK & OFDM) |
| 9 | HT/AMPDU diiklankan | net80211 bisa memilih jalur HT/agregasi yang belum didukung | HT dimatikan untuk uji pertama (802.11g) |
| 10 | Scan berputar tanpa jeda | TX probe terus-menerus; dongle pernah `hardware connection lost` | jeda 800 ms di kanal 1 pada siklus scan kedua dst. |
| 11 | Wrapper `probe`/`free` | field tak terinisialisasi; interface retain bocor | `init()` + `free()` eksplisit |
| 12 | `install-m4-esp.sh` | config rusak tidak dipulihkan; M1 dihapus sebelum M4 tersalin | rollback otomatis + verifikasi executable + urutan salin |
| 13 | Laporan C2H/TX report masuk parser frame | sampah firmware diproses net80211 (risiko panic) | bit `R92E_RXDW2_RPT_C2H` dibuang; `CCX_RPT` dimatikan sampai rate control ada |

### Status build setelah audit

```
./scripts/build-m4.sh                    rc=0
kextutil -t ...                          hanya "not approved to load"
nm -u | grep cxa_pure_virtual            hanya dari vtable ItlHalService
                                         (abstract base, tidak pernah dipanggil)
./scripts/run-tests.sh                   PASS (semua suite)
```

### 10b. Audit putaran kedua — TX/RX/teardown/lifecycle (22 Okt 2026, malam)

Audit lanjutan setelah tabel di atas, fokus pada jalur yang **pasti dieksekusi
saat boot dan asosiasi pertama**. Semua bug di bawah sudah diperbaiki dan
build/tes ulang lulus (`m4build18.log`, `runtests4.log`).

| # | Temuan | Dampak | Perbaikan |
|---|---|---|---|
| 14 | **Descriptor TX data salah offset** — `DRVRATE`/`HWSEQ` ditulis ke `txdw3` (byte 12–13) | Chip membaca `txdw3` sebagai bit HWSEQ/seq; frame data bisa dikirim tanpa rate/seq yang benar → TX macet | tulis ke **`txdw4`** (byte 16–19): non-QoS `0x0180` (HWSEQ\|DRVRATE) + `txdseq=0x8000`; QoS `0x0140` + seq asli; `txdw5=0` CCK 1 Mb/s. Sesuai `urtwn_tx_fill_desc` |
| 15 | `mbuf_pullup()` gagal lalu `mbuf_freem(m)` | API Apple **membebaskan seluruh chain** saat gagal → **double free** → panic | jangan free lagi; cukup lepas referensi node |
| 16 | `ieee80211_encrypt()` gagal lalu `mbuf_freem(m)` | Encrypt **sudah** membebaskan `m0` (sukses & gagal) → double free | hapus `mbuf_freem` di jalur gagal |
| 17 | Filter TX-report memakai `RXDW2` bit 28 (field C2H milik 92E) | Frame 88E valid bisa ikut dibuang / report lolos | hanya `RXDW3` bit 14–15 (OpenBSD 88E); bit C2H dihapus |
| 18 | RSSI PHYST dicek di bit 20 | PHY status tidak pernah terdeteksi → RSSI selalu -95 → menu sinyal palsu | `R92C_RXDW0_PHYST` = **bit 26** (`0x04000000`) |
| 19 | RSSI dBm mentah dimasukkan ke `rxi_rssi` | `ni_rssi` bertipe `u8` → nilai negatif wrap (mis. -50 → 206) | normalisasi `clamp(dbm + 100, 0, ic_max_rssi)` seperti iwm |
| 20 | Frame CRC/ICV error tetap diproses | net80211 menerima frame korup (risiko state aneh) | drop sebelum `ieee80211_inputm`, sama seperti OpenBSD |
| 21 | `releaseAll()` melepas `fHalService` sebelum timer | `watchdogAction`/`fakeScanDone` menembak HAL yang sudah bebas → NULL deref | cancel/disable/remove **semua timer** dulu, baru release HAL; `_fCommandGate`/`_fWorkloop` dilepas terakhir |
| 22 | Watchdog di workloop terpisah | `ieee80211_watchdog` mengubah state/free node **bersamaan** dengan RX di workloop IO80211 → race | watchdog dipindah ke `_fWorkloop` yang sama dengan RX + timer scan |
| 23 | `watchdogCb` hanya membersihkan `if_timer` | Auth/assoc timeout tidak pernah retry → state macet di AUTH | panggil `ieee80211_watchdog(ifp)` (retry 3× lalu scan) |
| 24 | `free()` sebelum `initWithController` | `free()` bisa jalan saat base member belum di-zero → release pointer sampah | `ItlHalService::init()` men-zero semua member; `ItlRtl8188E::init()` memanggil `super::init()` |
| 25 | Teardown RX tidak sinkron | callback USB bisa menyentuh `fRxLock`/`fRxTimer` yang sudah dibebaskan | `fRxRunning=false` + callback null + `bulkClose()` (abort sinkron) **sebelum** free timer/queue/lock |
| 26 | `restore-test-esp.sh` lama | Tidak mengenal M4; bisa menyisakan bundle/entri atau salah hapus boot-arg | deteksi ESP eksplisit, hapus M1+M4, parsing boot-arg per token (flag & nilai terpisah), backup config, fallback |
| 27 | Tidak ada diagnostik saat boot | `log show` rusak di mesin ini → tidak tahu sejauh mana driver jalan | properti ioreg: `RTL8188E-stage`, `RTL8188E-scan`, `RTL8188E-first-rx`, `RTL8188E-mac` |

Catatan hasil audit OpenBSD `rtwn_newstate`: untuk STA data dasar, RCR tanpa
`CBSSID_DATA/BCN` (menerima semua BSSID), `RXFLTMAP2=0xffff`, `TXPAUSE=0`,
MACID 0/BSSID 0 sudah cukup. Penulisan TSF/RAID/rate-init dan switching gain
scan↔link sengaja **belum** ditambahkan karena belum terbukti diperlukan.


### Yang masih belum bisa diverifikasi tanpa hardware

1. Urutan init chip lewat **IOUSBHostInterface** (M1 memakai jalur device;
   M4 memakai interface) — kode USB sama, tapi provider berbeda.
2. `setChannel` (control transfer) dari workloop IO80211 saat scan.
3. Asosiasi open/WPA2, 4-way handshake software, DHCP.
4. Daya tahan bus USB saat scan + TX data.

Semua poin di atas hanya bisa dibuktikan dengan **satu reboot** setelah dongle
tercolok. Rollback tetap `sudo ./scripts/restore-test-esp.sh`.
