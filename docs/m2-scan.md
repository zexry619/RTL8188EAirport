# M2 — Scan 802.11 dari user space (SELESAI 22 Okt 2026)

**Status: ✅ TERBUKTI DI HARDWARE.** Dongle RTL8188EUS (2357:010c) mengirim
probe request dan menerima beacon/probe response nyata; SSID sekitar terbaca
dari user space tanpa reboot, tanpa kext, tanpa aplikasi pihak ketiga.

```
$ sudo ./tools/rtwn_probe --scan 1 13
── Init untuk scan ──
  power-on ✓
  DMA/LLT ✓
  firmware ✓ (15262 byte)
  EFUSE ✓ MAC=98:03:8e:9e:bc:24 xtal=0x2f
── MAC/BB/RF init (tabel OpenBSD) ──
  MAC: 91 register
  BB : 320 register
  RF : 91 register
  core config: 0
── Scan kanal 1..13 ──
  ch  1  90:56:de:46:a7:24  ZEKRI.NET
  ch  1  28:6e:da:06:4e:18  Homesans
  ch  4  50:5b:1d:b6:0c:32  KUSUMA
  ch  5  a4:f3:3b:14:d9:b4  Qurha.com
  ch  6  24:46:e4:e2:cd:cc  RIYAN
  ch  7  0c:37:47:9c:62:77  Ramodal
  ch  9  50:5b:1d:b6:ab:fa  Sutopo
  ch 11  a6:d9:90:4d:91:33  Vii
── Hasil: 13 probe TX, 32 frame, 32 SSID ──
```

Bukti TX benar-benar bekerja (bukan cuma mendengar beacon pasif): pada kanal 1
tertangkap **probe response** dengan `addr1 = 98:03:8e:9e:bc:24` (MAC kita):

```
frame @56: 50 00 3a 01 | 98 03 8e 9e bc 24 | 90 56 de 46 a7 24 ...
           ^fc=0x0050  ^addr1 = MAC kita   ^addr2 = BSSID AP
```

---

## 1. Urutan yang terbukti

Urutan ini **persis** yang dipakai `tools/rtwn_probe --scan` dan sudah
diverifikasi di chip:

| # | Langkah | Fungsi |
|---|---|---|
| 1 | Power-on flow vendor (11 langkah) | `rtwn_seq_power_on` |
| 2 | LLT + queue/page DMA | `rtwn_seq_dma_init(..., ntx=3)` |
| 3 | Upload firmware 8051 (15.262 B) | `rtwn_seq_load_firmware` |
| 4 | Baca EFUSE (offload) → MAC + xtal | `rtwn_seq_read_efuse_offload` |
| 5 | Tabel MAC (91 register) | `rtwn_init_mac_8188e` |
| 6 | Tabel BB (320 register) + AGC + xtal | `rtwn_init_bb_8188e` |
| 7 | Tabel RF chain 0 (91 register) | `rtwn_init_rf_8188e` |
| 8 | Core config (Rx filter, EDCA, TX/RX enable, agregasi) | `rtwn_core_config_8188e` |
| 9 | Set kanal | `rtwn_set_chan_8188e` |
| 10 | TX probe request (bulk OUT) | `rtwn_usb_build_mgmt_tx_desc` |
| 11 | RX beacon/probe response (bulk IN) | `rtwn_usb_parse_rx_desc` + `rtwn_parse_ssid` |

Sumber tabel: `src/urtwn/r92creg.h` (`rtl8188eu_mac`, `rtl8188eu_bb_prog`,
`rtl8188eu_rf_prog`) — tabel OpenBSD yang sudah terbukti untuk 8188EU.
Ini menghindari ambiguitas tabel "MP" vendor (beberapa tabel vendor berisi
alamat RF 0x00–0x1b yang harus ditulis lewat LSSI, bukan sebagai register MAC).

---

## 2. Format yang dipakai

### TX (bulk OUT, 32 byte descriptor + frame)

```
offset 0  txdw0  PKTLEN | OFFSET=32 | OWN | FSG | LSG | BMCAST
offset 4  txdw1  MACID=0 | QSEL=MGNT(0x12) | RAID=11B(6)
offset 8  txdw2  0
offset 12 txdw3  0
offset 14 txdseq HWSEQEN (0x8000)
offset 16 txdw4  DRVRATE (0x100) | HWSEQ (0x80)
offset 20 txdw5  DATARATE = 0 (CCK 1 Mb/s)
offset 28 txdsum checksum XOR 16 word pertama (word 14 = 0 saat hitung)
```

Frame probe request: header 24 byte + SSID element + supported rates +
DS parameter (kanal). Destination broadcast, addr3 broadcast.

### RX (bulk IN)

```
+0   24 byte  descriptor (rxdw0..rxdw5)
+24  N byte   PHY status (INFOSZ = ((rxdw0 >> 16) & 0xf) * 8)
+..  pktlen   frame 802.11 (mulai dengan fc; beacon = 0x80 0x00)
```

`infosz` **wajib** dibaca dari descriptor (bukan asumsi 2 byte). Pada
capture nyata `infosz = 32` (RX_DRVINFO_SZ = 4) dan frame mulai di offset 56.

Satu transfer bulk IN dapat berisi **beberapa paket** (agregasi RXDMA);
parser mengiterasi `offset += 24 + infosz + pktlen`.

---

## 3. Bug yang ditemukan & diperbaiki pada M2

| # | Gejala | Akar | Perbaikan |
|---|---|---|---|
| 1 | SSID jadi sampah / `\xNN` | `rtwn_parse_ssid` mulai membaca elemen di offset 24; beacon punya **12 byte field tetap** (timestamp+interval+capability) → elemen mulai offset **36** | `rtwn_parse_ssid` memilih offset body berdasarkan subtype (beacon/probe response = 36, probe request = 24) |
| 2 | Kanal tidak berpengaruh | `rtwn_set_chan_8188e` membaca RF CHNLBW dulu; readback LSSI bisa salah | Verifikasi: RF18 berubah `0x007c01`…`0x007c0b` → jalur tulis benar; sisa paket lama berasal dari agregasi RX, bukan kanal |
| 3 | BSSID sama muncul di banyak kanal | RX aggregation menahan paket kanal sebelumnya | Drain bulk IN setelah ganti kanal, sebelum TX probe |
| 4 | Descriptor TX ditolak chip | checksum descriptor belum dihitung | `rtwn_usb_tx_checksum` (XOR 16 word, simpan di word 14) |

Test regresi: `tools/test_80211.c` sekarang memakai layout beacon yang benar
(offset 36) + kasus probe request, probe response, SSID hidden, dan element
terpotong.

---

## 4. Cara menjalankan

```bash
# build alat (tidak menyentuh kernel)
./scripts/build-probe.sh

# scan semua kanal 2,4 GHz (butuh sudo; driver vendor harus mati)
sudo ./tools/rtwn_probe --scan 1 13

# scan satu kanal
sudo ./tools/rtwn_probe --scan 6

# dump frame mentah (debug parser)
sudo env RTWN_DUMPFRAME=1 ./tools/rtwn_probe --scan 6 6
```

Syarat: dongle tidak dipegang driver kernel. Kext kita self-detach; tunggu
±10 detik setelah colok. Jika perlu:
`sudo kextunload -b io.github.zekri.RTL8188EUM1`.

---

## 5. ⚠️ Risiko yang masih terbuka: dongle bisa lepas dari bus

Pada salah satu pengujian berulang, macOS mencatat:

```
AppleUSBHostPort::terminateDevice: destroying 0x2357/010c/0000 (802.11n NIC):
    hardware connection lost
```

Artinya perangkat **hilang dari bus USB** (bukan panic, bukan bug kernel).
Dugaan penyebab (perlu validasi):

1. **Daya TX terlalu tinggi** — tabel `phy_reg_pg` / `txpwr_lmt` vendor belum
   diterapkan, jadi PA bisa memancar di atas batas yang diizinkan; lonjakan
   arus → port melakukan proteksi.
2. Firmware di-upload ulang setiap kali probe dijalankan (power-on + firmware
   berulang), sementara chip belum di-quiesce dengan benar di akhir.
3. Kontak fisik dongle.

Mitigasi yang direncanakan (sebelum uji berikutnya):

- Terapkan batas TX power dari EFUSE (`txpwr_lmt`) atau set register TX AGC
  ke nilai konservatif.
- Tambahkan **quiesce** di akhir scan: `R92C_TXPAUSE = 0x0f`, radio off
  (`rtwn_flow_radio_off_8188e`), lalu tutup device.
- Jangan upload firmware ulang jika `MCUFWDL` menunjukkan firmware sudah
  berjalan (deteksi state).

Sampai mitigasi ini ada, **jangan menjalankan scan berulang-ulang tanpa jeda**,
dan siapkan akses fisik untuk mencabut-colok dongle.

---

## 6. Hubungan ke M3/M4

M2 membuktikan seluruh jalur radio (TX/RX 802.11) bekerja. Yang tersisa untuk
M4 bukan lagi protokol chip, melainkan **integrasi**:

- pindahkan urutan di atas ke driver kernel (kode portabel sudah sama),
- sambungkan ke `ItlHalService` (net80211 + AirportItlwm) atau tulis
  `IO80211Controller` minimal,
- scan di M4 seharusnya hanya memanggil `run_scan` yang sama dan mengisi
  hasil ke cache scan IO80211.

Lihat `docs/m2-m4-contract.md` dan `docs/m4-airport-integration.md`.
