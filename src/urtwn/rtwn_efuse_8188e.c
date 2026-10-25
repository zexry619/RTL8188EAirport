/*
 * rtwn_efuse_8188e.c — baca EFUSE 8188E (USB) dengan layout register 8188E.
 *
 * ⭐ TEMUAN PENTING: layout register EFUSE untuk 8188E BERBEDA dari OpenBSD!
 *
 *   OpenBSD (gaya 92C, dipakai rtwn_efuse_read_1):
 *      write  0x030 = (addr << 8) & 0x3FF00, tanpa bit VALID
 *      poll   0x030 sampai bit31 (VALID) set
 *      data   0x030
 *
 *   8188E USB (vendor, EFUSE_OneByteRead @0x119c0b):
 *      write  0x031 = addr & 0xFF                  (address low)
 *      write  0x032 = (0x032 & 0xFC) | ((addr >> 8) & 3)  (address high)
 *      write  0x033 = 0x033 & 0x7F                  (clear busy)
 *      poll   0x033 sampai bit7 CLEAR  (maks 100 × 1 s)
 *      data   0x030
 *
 * Menulis address ke 0x030 ( ala OpenBSD ) tidak berguna pada 8188E karena
 * 0x030 adalah register DATA — itu sebabnya pembacaan EFUSE versi OpenBSD
 * selalu timeout pada chip ini.
 */

#include "rtwn_seq.h"
#include "rtwn_platform.h"
#include <string.h>


/* Register EFUSE 8188E. */
#define EFUSE_DATA_8188E   0x030
#define EFUSE_ADDR_LO_8188E 0x031
#define EFUSE_ADDR_HI_8188E 0x032
#define EFUSE_STAT_8188E   0x033
#define EFUSE_ACCESS_8188E 0x106
#define EFUSE_ACCESS_ON    0x69

/*
 * Baca satu byte EFUSE (offset 0..0x3FF).
 * Return 0 = sukses, -1 = timeout.
 */
int rtwn_efuse_read1_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t offset, uint8_t *out)
{
	uint8_t v;
	int i;

	/* Alamat offset (low + high 2 bit). */
	rtwn_seq_raw_write1(rd, wr, ctx, EFUSE_ADDR_LO_8188E, (uint8_t)(offset & 0xff));

	v = rtwn_seq_raw_read1(rd, wr, ctx, EFUSE_ADDR_HI_8188E);
	rtwn_seq_raw_write1(rd, wr, ctx, EFUSE_ADDR_HI_8188E,
	    (uint8_t)((v & 0xfc) | ((offset >> 8) & 0x03)));

	/* Bersihkan bit "busy". */
	v = rtwn_seq_raw_read1(rd, wr, ctx, EFUSE_STAT_8188E);
	rtwn_seq_raw_write1(rd, wr, ctx, EFUSE_STAT_8188E, (uint8_t)(v & 0x7f));

	/*
	 * Tunggu sampai bit7 MENYALA (= pembacaan selesai).
	 *
	 * ‑‑‑ KESALAHAN POLARITAS (ditemukan via RE 22 Okt 2026) ‑‑‑
	 *
	 * Vendor (RtWlanU :: EFUSE_OneByteRead, 0x119c0b):
	 *
	 *     PlatformIOWrite1Byte(0x033, v & 0x7f);      // clear bit7 = MULAI
	 *     cVar5 = PlatformEFIORead1Byte(0x033);
	 *     if (-1 < cVar5) {                           // signed char!
	 *         do { stall(1000);
	 *              cVar5 = PlatformEFIORead1Byte(0x033);
	 *         } while (-1 < cVar5);                  // ← tunggu bit7 MENYALA
	 *         if (99 < counter) { *out = 0xff; error = 2; return FAIL; }
	 *     }
	 *     *out = PlatformEFIORead1Byte(0x030);         // DATA
	 *
	 * Perhatikan: `cVar5` adalah `char` (signed) → `-1 < cVar5` berarti
	 * bit7 CLEAR. Loop berjalan selama bit7 masih CLEAR dan berhenti
	 * ketika bit7 MENYALA. Jadi bit7 = "SIAP", bukan "busy".
	 *
	 * Versi lama kita menunggu bit7 MATI → selalu keluar dari loop
	 * lebih awal / timeout → data EFUSE stale atau 0xff.
	 *
	 * Delay: vendor stall(1000) = 1000 µs = 1 ms. Kita pakai
	 * RTWN_DELAY_MS(1) (sleep, bukan busy-wait) — 1 ms juga.
	 */
	for (i = 0; i < 100; i++) {
		v = rtwn_seq_raw_read1(rd, wr, ctx, EFUSE_STAT_8188E);
		if ((v & 0x80) != 0) {
			break;			/* bit7 menyala = siap */
		}
		RTWN_DELAY_MS(1);
	}
	if (i >= 100) {
		return -1;
	}

	*out = rtwn_seq_raw_read1(rd, wr, ctx, EFUSE_DATA_8188E);
	return 0;
}

/*
 * Baca seluruh map EFUSE 8188E dengan cara vendor (byte per byte),
 * lalu parse dengan parser yang sama dengan vendor.
 *
 * PENTING: urutan byte mentah yang DIHARAPKAN parser adalah "header blok +
 * data". Pembacaan byte-per-byte di sini menghasilkan stream mentah yang
 * POSISI-nya sama dengan yang dipakai parser (header di byte 0, lalu data).
 *
 * Return 0 = sukses.
 */

/*
 * Formatter hex tanpa stdio (kernel tidak boleh pakai stdio.h).
 */
static void
m1_hex(char *dst, size_t cap, const uint8_t *src, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	if (!dst || !src || cap < n * 2 + 1) {
		return;
	}
	for (i = 0; i < n; i++) {
		dst[i * 2] = hx[(src[i] >> 4) & 0xf];
		dst[i * 2 + 1] = hx[src[i] & 0xf];
	}
	dst[n * 2] = 0;
}

int rtwn_seq_read_efuse_direct_8188e(rtwn_seq_result_t *r, rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx)
{
	uint8_t raw[256];
	uint8_t rom[512];
	uint16_t i;
	int read_ok = 1;

	/* Aktifkan akses EFUSE. */
	rtwn_seq_raw_write1(rd, wr, ctx, EFUSE_ACCESS_8188E, EFUSE_ACCESS_ON);

	/*
	 * Stream mentah 8188E: setiap blok diawali 1 byte header
	 * (mask + nibble offset) diikuti byte data. Ukuran totalnya
	 * bergantung mask, jadi kita baca bertahap.
	 *
	 * Strategi: baca 256 byte berurutan (offset 0..255) — sama seperti
	 * OpenBSD — lalu parse. Untuk chip 8188E ini setara dengan
	 * "InitOffload_ReadEfuse" yang juga mengisi buffer 256 byte.
	 */
	for (i = 0; i < sizeof(raw); i++) {
		uint8_t v;
		if (rtwn_efuse_read1_8188e(rd, wr, ctx, i, &v) != 0) {
			read_ok = 0;
			break;
		}
		raw[i] = v;
	}
	/*
	 * DIAGNOSTIK (22 Okt 2026): berapa byte yang berhasil terbaca?
	 * Data ini diperlukan untuk membedakan:
	 *   - chip tidak merespons sama sekali (0 byte)
	 *   - chip merespons sebagian lalu macet (N byte)
	 */
	{
		char dbg[32];
		uint8_t cnt = (uint8_t)i;

		m1_hex(dbg, sizeof(dbg), &cnt, 1);
		rtwn_seq_diag_set(ctx, "efuse-direct-bytes", dbg);
		m1_hex(dbg, sizeof(dbg), raw, 8);
		rtwn_seq_diag_set(ctx, "efuse-direct-head", dbg);
		{
			/* Dump penuh untuk analisis parser offline. */
			char big[560];

			m1_hex(big, sizeof(big), raw, 256);
			rtwn_seq_diag_set(ctx, "efuse-direct-dump", big);
		}
	}

	/* Kembalikan akses EFUSE. */
	rtwn_seq_raw_write1(rd, wr, ctx, EFUSE_ACCESS_8188E, 0x00);

	if (!read_ok) {
		return -1;
	}

	memset(rom, 0xff, sizeof(rom));
	rtwn_seq_efuse_parse(raw, (uint16_t)sizeof(raw), rom);

	/*
	 * Validasi (dari decompilation vendor InitOffload_ReadEfuse_8188EU):
	 * byte pertama image ROM hasil parse harus 0x29 (' )').
	 * Kalau tidak → image tidak valid (EFUSE tidak terbaca / mode salah).
	 */
	if (rom[0] != 0x29) {
		return -1;
	}

	/*
	 * Validasi KEDUA (lebih kuat): image EFUSE 8188E menyimpan USB
	 * VID/PID di offset 0xD0/0xD2 (lihat `struct r88e_rom` FreeBSD,
	 * sys/dev/rtwn/rtl8188e/r88e_rom_image.h):
	 *
	 *   0xD0 vid (2 byte LE)   0xD2 pid (2 byte LE)
	 *   0xD4 usb_opt           0xD7 macaddr[6]
	 *
	 * Kalau yang terbaca bukan 2357:010c → image bukan milik chip ini
	 * (EFUSE salah baca / mode offload salah).
	 */
	if (rom[0xD0] != 0x57 || rom[0xD1] != 0x23 ||
	    rom[0xD2] != 0x0c || rom[0xD3] != 0x01) {
		return -1;
	}

	memcpy(r->mac, rom + 215, 6);
	r->xtal = rom[185];
	r->channel_plan = rom[184];
	r->regulatory = (uint8_t)(rom[193] & 0x07);
	r->efuse_ok = 1;
	r->step = 2;
	return 0;
}
