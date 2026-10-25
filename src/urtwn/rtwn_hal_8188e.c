/*
 * rtwn_hal_8188e.c — penerapan tabel parameter HAL ("MP") untuk 8188E.
 *
 * sourced dari decompilation vendor RtWlanU (Ghidra 12.1.4, 22 Okt 2026):
 *   odm_read_and_config_mp_8188e_mac_reg   (0x1fa8e4)
 *   odm_read_and_config_mp_8188e_phy_reg   (0x1f6682)
 *   odm_read_and_config_mp_8188e_phy_reg_pg(0x1f6762)
 *   odm_read_and_config_mp_8188e_agc_tab   (0x1f...)
 *   odm_read_and_config_mp_8188e_radioa    (0x1f...)
 *
 * File parameter gaya Windows (rtl8188E\MAC_REG.txt) tidak ada di macOS,
 * jadi yang dipakai driver vendor adalah TABEL BUILT-IN yang tertanam di
 * binary — tabel itulah yang kita ekstrak ke rtwn_mp_8188e.c.
 *
 * ── FORMAT ENTRI (TERBUKTI dari byte mentah) ──
 *
 *     offset 0 : uint32 odd   (reg ATAU descriptor bersyarat)
 *     offset 4 : uint8  val   (nilai)
 *     offset 5 : 3 byte padding
 *     → 8 byte per entri
 *
 * ── MAKNA DESCRIPTOR `odd` (dari loop vendor) ──
 *
 *     odd <  0x40000000        → TULIS BIASA: reg = odd, nilai = val
 *     odd >= 0x80000000        → entri KONTROL
 *         (odd >> 28) & 3 == 3  → "resume": apply = true, cond = false
 *         (odd >> 28) & 3 == 2  → "toggle": apply = !cond
 *     odd >= 0x40000000 < 0x80000000 → entri BERSYARAT: bandingkan dengan
 *                                    field MAC di adapter (dm+0x60, 8 byte)
 *
 * ── CABANG BERSYARAT ──
 *
 * Entri 0x4xxxxxxx membandingkan 8 byte di dm+0x60 dengan pola yang
 * disandikan di entri sebelumnya. Byte ganjil (mask2) dan byte genap
 * (mask1) dibandingkan terpisah; byte yang di-mask 0xff = wildcard.
 *
 * Catatan: dm+0x60 = MAC address 6 byte + 2 byte tambahan. Mask1 memakai
 * byte {0,2,4,6}, mask2 memakai byte {1,3,5,7}.
 */

#include "rtwn_hal_8188e.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Konteks penerapan
 * ------------------------------------------------------------------------ */

void
rtwn_hal_init(rtwn_hal_ctx_t *hc, rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	if (!hc) {
		return;
	}
	memset(hc, 0, sizeof(*hc));
	hc->rd = rd;
	hc->wr = wr;
	hc->ctx = ctx;
}

/*
 * Isi 8 byte "MAC window" yang dipakai untuk cabang bersyarat.
 *
 * Layout dm+0x60 (dari decompilation):
 *   +0x60 : MAC byte 0..1  (LE uint16)
 *   +0x62 : MAC byte 2..3
 *   +0x64 : MAC byte 4..5
 *   +0x66 : 2 byte tambahan (byte 6..7 dari window)
 *
 * Kita isi dengan MAC 6 byte + 2 byte nol; byte 6..7 di mask2 di wildcard
 * (0xff) pada sebagian besar entri, jadi tidak berpengaruh.
 */
void
rtwn_hal_set_mac(rtwn_hal_ctx_t *hc, const uint8_t mac[6])
{
	if (!hc || !mac) {
		return;
	}
	memcpy(hc->macwin, mac, 6);
	hc->macwin[6] = 0;
	hc->macwin[7] = 0;
}

/* ------------------------------------------------------------------------
 * Penerapan satu tabel
 * ------------------------------------------------------------------------ */

/*
 * Bandingkan 8 byte window MAC dengan pola dari entri bersyarat.
 *
 * Mengembalikan true kalau COCOK (pola terpenuhi → entri berlaku).
 *
 * src/saved_val & saved_odd menyimpan "pola" yang disandikan vendor:
 *   saved_odd  = entri kontrol terakhir (0x8xxxxxxx / 0x9xxxxxxx)
 *   saved_val  = nilai byte dari entri itu
 *
 * Yang dibandingkan:
 *   uVar9 = mask byte dari saved_odd (low 8 bit, dengan bit 1/2/3
 *           menentukan byte mana yang jadi wildcard 0xff)
 *   mask1 = byte genap window MAC  (0,2,4,6)
 *   mask2 = byte ganjil window MAC  (1,3,5,7)
 *
 * Kondisi gagal (dari vendor):
 *   (uVar9 & (mask1 ^ saved_val)) != 0   →  mismatch byte genap
 *   (uVar9 & (mask2 ^ cur_val))   != 0   →  mismatch byte ganjil
 */
static bool
rtwn_hal_cond_match(const rtwn_hal_ctx_t *hc, uint32_t saved_odd,
    uint8_t saved_val, uint8_t cur_val)
{
	uint32_t mask;
	uint32_t mask1, mask2;

	/* Bangun mask dari bit low 8 dari saved_odd.
	 * bit1 → byte1 wildcard, bit2 → byte2, bit3 → byte3, dst.
	 * byte0 selalu ikut (tidak ada bit untuk itu di decompilation),
	 * jadi selalu tepat 1 byte. */
	uint8_t b0 = (uint8_t)(saved_odd & 0xff);

	mask = b0;
	if (saved_odd & 2) {
		mask |= 0xff00u;
	}
	if (saved_odd & 4) {
		mask |= 0xff0000u;
	}
	if (saved_odd & 8) {
		mask |= 0xff000000u;
	}

	/* mask1: byte genap window (0,2,4,6) */
	mask1 = (uint32_t)(hc->macwin[0] & 0xff) |
	    ((uint32_t)(hc->macwin[2] & 0xff) << 8) |
	    ((uint32_t)(hc->macwin[4] & 0xff) << 16) |
	    ((uint32_t)(hc->macwin[6] & 0xff) << 24);

	/* mask2: byte ganjil window (1,3,5,7) */
	mask2 = (uint32_t)(hc->macwin[1] & 0xff) |
	    ((uint32_t)(hc->macwin[3] & 0xff) << 8) |
	    ((uint32_t)(hc->macwin[5] & 0xff) << 16) |
	    ((uint32_t)(hc->macwin[7] & 0xff) << 24);

	if ((mask & (mask1 ^ (uint32_t)saved_val)) != 0) {
		return false;
	}
	if ((mask & (mask2 ^ (uint32_t)cur_val)) != 0) {
		return false;
	}
	return true;
}

/*
 * Terapkan satu tabel MP.
 *
 * Mirip loop vendor:
 *     apply = true; cond = false; saved_val = 0; saved_odd = 0;
 *     for each entry:
 *         odd = e.odd; val = e.val;
 *         sv = saved_val; so = saved_odd;
 *         if (odd < 0x40000000) {
 *             if (apply) write(odd, val);
 *             else apply = false;
 *         } else if ((int32_t)odd < 0) {            // 0x8xxxxxxx..
 *             t = (odd >> 28) & 3;
 *             if (t == 3) { apply = true; cond = false; }
 *             else { sv = val; so = odd;
 *                      if (t == 2) { apply = !cond; sv = saved_val; so = saved_odd; } }
 *         } else if (odd & 0x40000000) {           // 0x4xxxxxxx..
 *             if (cond) { apply = false; goto next; }
 *             if (!rtwn_hal_cond_match(hc, saved_odd, saved_val, val)) {
 *                 apply = false; goto next;
 *             }
 *             cond = true;
 *             if (saved_odd & 0xf) {
 *                 if (!rtwn_hal_cond_match(hc, saved_odd, saved_val, val)) {
 *                     apply = false; goto next;
 *                 }
 *             }
 *             apply = true;
 *         }
 *         saved_val = sv; saved_odd = so;
 *     }
 *
 * Return: jumlah register yang benar-benar ditulis.
 */
int
rtwn_hal_apply_table(rtwn_hal_ctx_t *hc, const rtwn_mp_entry_t *tab, size_t n)
{
	int applied = 0;
	bool apply = true;
	bool cond = false;
	uint8_t saved_val = 0;
	uint32_t saved_odd = 0;
	size_t k;

	if (!hc || !tab || !hc->rd || !hc->wr) {
		return 0;
	}

	for (k = 0; k < n; k++) {
		uint32_t odd = tab[k].odd;
		uint8_t  val = tab[k].val;
		uint8_t  sv = saved_val;
		uint32_t so = saved_odd;

		if (odd < RTWN_MP_PLAIN_MAX) {
			/* Tulis biasa. */
			if (apply) {
				/* Semua entri MP = tulis 1 byte ke 1 register. */
				if (hc->wr(hc->ctx, (uint16_t)odd, &val, 1) == 0) {
					applied++;
				}
			} else {
				apply = false;
			}
		} else if ((int32_t)odd < 0) {
			/* Entri kontrol (0x8xxxxxxx / 0x9xxxxxxx / 0xCxxxxxxx). */
			uint32_t t = (odd >> 28) & 3;

			if (t == 3) {
				apply = true;
				cond = false;
			} else {
				sv = val;
				so = odd;
				if (t == 2) {
					apply = !cond;
					sv = saved_val;
					so = saved_odd;
				}
			}
		} else if (odd & 0x40000000u) {
			/* Entri bersyarat (0x4xxxxxxx). */
			if (cond) {
				apply = false;
				goto next;
			}
			if (!rtwn_hal_cond_match(hc, saved_odd, saved_val, val)) {
				apply = false;
				goto next;
			}
			cond = true;
			if (saved_odd & 0xf) {
				if (!rtwn_hal_cond_match(hc, saved_odd,
				    saved_val, val)) {
					apply = false;
					goto next;
				}
			}
			apply = true;
		}
next:
		saved_val = sv;
		saved_odd = so;
	}

	return applied;
}
