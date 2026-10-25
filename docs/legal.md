# Catatan Lisensi & Legal

> Bukan nasihat hukum. Ini ringkasan teknis agar keputusan produk tidak menebak.

---

## 1. Komponen & lisensinya

| Komponen | Lisensi | Boleh dipakai ulang? | Boleh didistribusikan ulang? |
|---|---|---|---|
| OpenBSD `if_urtwn.c`, `rtwn.c`, `rtwnvar.h`, `r92creg.h` | ISC | Ya | Ya, wajib sertakan notice ISC |
| OpenBSD `net80211` (melalui `itl80211`) | ISC (asal) | Ya | Ya + notice |
| `laobamac/itlwm` (`AirportItlwm/`) | GPL-2.0 | Ya | Ya, **hanya** dengan source lengkap & GPL-2.0 |
| `AirPort-RTW` (`RTW88USBDevice.cpp` dll) | GPL-2.0 | Ya (pola) | Sama syaratnya |
| `MacKernelSDK` | BSD-3-Clause + lisensi Apple | Ya (toolchain) | Ya |
| Firmware `rtl8188eufw.bin` | Lisensi firmware Realtek (lihat `LICENCE.rtlwifi_firmware`) | Umumnya ya | Umumnya ya, **verifikasi** |
| Binary vendor (`RtWlanU`, `StatusBarApp`) | Tertutup, proprietary | Hanya untuk observasi/interoperability | ❌ tidak boleh disalin/didistribusikan |
| Header IO80211 Apple (`include/Airport/*`) | Apple Private Source / APSL | Referensi pribadi | ❌ tidak boleh dipublikasikan |

## 2. Dampak GPL

Karena frontend diambil dari `itlwm` (GPL-2.0), proyek ini **wajib** GPL-2.0:
* semua file yang kita tulis ikut GPL-2.0;
* distribusi harus menyertakan source lengkap (termasuk patch terhadap upstream);
* kode ISC (OpenBSD) boleh digabung ke GPL (satu arah, sah).

**Masalah nyata:** menyertakan header Apple yang privat dalam source GPL melanggar
persyaratan lisensi Apple (Apple Public Source License / proprietary header).
Praktik yang umum di komunitas: header privat **tidak** ikut dipublikasikan, dan
build dilakukan oleh tiap pengguna (script fetch), atau distribute patch yang
menghasilkan header tersebut saat build.

## 3. Clean-room

Kalau someday perlu driver **yang boleh dipublikasikan** tanpa header Apple:
1. Tim A: mengumpulkan spesifikasi perilaku (dari dokumen, observasi, dan
   driver open-source yang berlisensi permissive).
2. Tim B: mengimplementasikan hanya dari spesifikasi, tanpa melihat kode tim A.
3. Dokumentasikan prosesnya (repo terpisah, commit history sebagai bukti).

## 4. Reverse engineering untuk interoperabilitas

Di banyak yurisdiksi (mis. EU Directive 2009/24/EC, US fair use) reverse engineering
untuk interoperabilitas diperbolehkan, **asalkan** hasil tidak menyalin kode berhak cipta
dan tidak melanggar kontrak lisensi. Yang jelas **tidak boleh**:
* menyalin potongan kode vendor ke dalam proyek kita;
* mendistribusikan binary turunan vendor;
* melewati proteksi DRM (TPM/secure boot).

## 5. Klausul "tidak boleh" (ringkas)

* ❌ Menyalin kode/firmware dari `RtWlanU` atau `StatusBarApp`.
* ❌ Mempublikasikan header Apple privat.
* ❌ Meng-girlocate GPL tanpa source.
* ❌ Menyebut produk ini "resmi"/"dibuat oleh Realtek" — ini proyek pihak ketiga.
* ✅ Menyebut "berbasis OpenBSD urtwn & net80211 (ISC), frontend diturunkan dari itlwm (GPL-2.0)".

## 6. Things to do before publishing

- [ ] `LICENSE` GPL-2.0 lengkap di root
- [ ] `NOTICE` berisi notice ISC OpenBSD & itlwm
- [ ] Header privat Apple tidak ikut ter-commit
- [ ] `scripts/fetch-upstream.sh` mengunduh header privat saat build
- [ ] Firmware beserta lisensinya & checksum
- [ ] Pernyataan "tidak berafiliasi dengan Realtek/Apple"
