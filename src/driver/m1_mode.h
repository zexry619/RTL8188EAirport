/*
 * m1_mode.h — mekanisme pemilihan mode M1.
 *
 * Mode diambil dari boot-argument (OpenCore: NVRAM → boot-args):
 *
 *   boot-args = "-rtl8188eum1_mode full"
 *
 *   - "full"     → power-on + upload firmware (MENULIS register chip).
 *                  Wajib dijalankan saat driver vendor Realtek nonaktif.
 *   - (default)  → READONLY: hanya membaca register chip, nol penulisan.
 *                  Aman dijalankan BERBARENGAN dengan driver vendor Realtek
 *                  (Wi-Fi + aplikasi vendor tetap normal).
 *
 * Boot-arg dipilih karena kernel XNU tidak punya POSIX file I/O
 * (tidak ada open()/read()).
 */

#ifndef RTL8188EU_M1_MODE_H
#define RTL8188EU_M1_MODE_H

#include <pexpert/pexpert.h>

#define M1_MODE_ARG "rtl8188eum1_mode"

enum {
	M1_MODE_READONLY = 0,
	M1_MODE_FULL     = 1,
	/*
	 * Tahap tengah: chip-id + EFUSE + power-on flow vendor (11 entri),
	 * lalu BERHENTI. Tanpa DMA/LLT, tanpa upload firmware.
	 *
	 * Kenapa perlu: dengan driver vendor nonaktif, chip BELUM berdaya
	 * → mode READONLY tidak bisa membaca apa pun yang bermakna.
	 * Tahap "poweron" adalah langkah paling kecil yang benar-benar
	 * menyalakan chip, dengan traffic USB paling sedikit.
	 */
	M1_MODE_POWERON  = 2,
	/*
	 * Tahap lanjut: power-on + DMA + firmware + EFUSE + **HAL tables**
	 * (PHY/RF/MAC register config dari vendor).
	 */
	M1_MODE_HAL      = 3
};

static inline int m1CurrentMode(void)
{
	char buf[32];

	if (PE_parse_boot_argn(M1_MODE_ARG, buf, sizeof(buf))) {
		if (buf[0] == 'f' || buf[0] == 'F') {
			return M1_MODE_FULL;
		}
	}
	return M1_MODE_READONLY;
}

/*
 * Mode efektif untuk build UJI (M1_ALLOW_USB_TRAFFIC=1).
 *
 * Build UJI secara default = FULL (menulis register chip).
 * Boot-arg "-rtl8188eum1_mode readonly" memaksa mode READONLY
 * (hanya baca) tanpa perlu build ulang — ini yang dipakai untuk
 * validasi bertahap.
 *
 * Build AMAN (traffic=0) TIDAK PERNAH bisa dinaikkan ke FULL:
 * aturan A3 (tidak boleh melakukan traffic USB saat boot).
 */
static inline int m1EffectiveMode(int buildAllowsTraffic)
{
	char buf[32];

	if (!buildAllowsTraffic) {
		return M1_MODE_READONLY;
	}

	/*
	 * ‑‑‑ MODE COMPILE-TIME (21 Okt 2026) ‑‑‑
	 *
	 * Boot-arg TIDAK bisa dipakai: PE_parse_boot_argn tidak menemukan
	 * arg kita meski kern.bootargs menunjukkannya (terbukti di lapangan:
	 * mode tetap "full" padahal boot-args berisi -rtl8188eum1_mode=poweron).
	 *
	 * Jadi mode dipilih saat BUILD lewat -DM1_TEST_MODE=N:
	 *     0 = readonly   (hanya baca register)
	 *     1 = poweron    (power-on + DMA + firmware + EFUSE, lalu berhenti)
	 *     2 = full       (lanjut ke bulk pipe + set MAC)
	 *
	 * Build AMAN (traffic=0) tetap terkunci di READONLY.
	 */
#ifdef M1_TEST_MODE
	return (M1_TEST_MODE == 0) ? M1_MODE_READONLY :
	    (M1_TEST_MODE == 1) ? M1_MODE_POWERON :
	    (M1_TEST_MODE == 2) ? M1_MODE_FULL : M1_MODE_HAL;
#else
	if (PE_parse_boot_argn(M1_MODE_ARG, buf, sizeof(buf))) {
		if (buf[0] == 'r' || buf[0] == 'R') {
			return M1_MODE_READONLY;
		}
		if (buf[0] == 'p' || buf[0] == 'P') {
			return M1_MODE_POWERON;
		}
		if (buf[0] == 'f' || buf[0] == 'F') {
			return M1_MODE_FULL;
		}
	}
	return M1_MODE_FULL;
#endif
}

#endif /* RTL8188EU_M1_MODE_H */
