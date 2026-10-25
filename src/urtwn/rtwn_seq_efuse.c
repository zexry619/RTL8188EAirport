/*
 * rtwn_seq_efuse.c — jalur baca EFUSE 8188E USB gaya driver vendor.
 *
 * PENTING: untuk 8188E USB, EFUSE TIDAK bisa dibaca langsung lewat
 * EFUSE_CTRL seperti di OpenBSD. Chip ini memakai jalur "offload": mesin
 * internal/8051 melakukan pembacaan dan mengembalikan (header, data)
 * lewat register 0x144/0x148.
 *
 * Sumber: decompilation Ghidra atas RtWlanU.kext
 *   - InitOffload_ReadEfuse_8188EU   (0xb1ea5)  → urutan offload
 *   - ParsingEfuseContent_8188EU      (0xb1bc2)  → parser (sudah di rtwn_seq.c)
 *   - Hal_EfuseParseXtal_8188E        (0x145358) → xtal dari rom[185]
 *   - HAL_8051Reset                   (0x71887)
 *   - HAL_8051PauseIOWrapper          (0x7169e)
 *   - HAL_8051EnableIOWrapper         (0x71799)
 *
 * Register yang dipakai (dari vendor):
 *   0x209  mode EFUSE (0xaa / 0xf4)      0x140  alamat window (16 bit)
 *   0x0F0  enable/p-flag                 0x143  status (bit0 = siap)
 *   0x088  trigger (bit1)                0x144  header (32 bit)
 *   0x088/0x106  reset 8051 / akses EFUSE (0x69)
 */

#include "rtwn_seq.h"
#include "rtwn_seq_efuse.h"
#include "rtwn_platform.h"
#include <string.h>

/* Register vendor (lihat header di atas). */
#define R92C_EFUSE_OFFLOAD_MODE   0x209
#define R92C_EFUSE_OFFLOAD_CTRL   0x0f0
#define R92C_EFUSE_OFFLOAD_TRIG   0x088
#define R92C_EFUSE_OFFLOAD_ADDR   0x140
#define R92C_EFUSE_OFFLOAD_STAT   0x143
#define R92C_EFUSE_OFFLOAD_HDR    0x144
#define R92C_EFUSE_OFFLOAD_DATA   0x148
#define R92C_EFUSE_ACCESS         0x106
#define R92C_SYS_FUNC_EN2         0x01c
#define R92C_AFE_XTAL_CTRL2       0x01d
#define R92C_CR2                  0x003

/* Helper: delay dalam ms. Di user space (probe) memakai usleep; di kernel
 * RTWN_DELAY_MS di-override ke IODelay/thread delay. */

/* --- internal helpers (menggunakan callback rtwn_seq.c) --- */

/* 8051 reset: pause IO wrapper → delay 50 ms → enable IO wrapper.
 * Untuk 8188EUSB (tipe 0x32) vendor memakai register 0x093 & 0xFE/0x01
 * dan reg 0x003 & 0xFB/0x04. */
/*
 * Pause 8051 — versi vendor (HAL_8051PauseIOWrapper, 0x7169e).
 *
 * ROOT CAUSE EFUSE (ditemukan lewat RE, 22 Okt 2026):
 *
 * Fungsi vendor memilih register berdasarkan cardType
 * (adapter + 0x3a0):
 *
 *   - cardType > 0x34            : tidak melakukan apa-apa
 *   - cardType di maskA (0x0e)  : GRP A
 *       0x1c &= ~0x02 ; 0x1d &= ~0x01 ; 0x03 &= ~0x04
 *   - cardType di maskB (0x0f/0x10) : GRP B
 *       0x1c &= ~0x02 ; 0x1d &= ~0x08 ; 0x03 &= ~0x04
 *   - cardType == 0x32           : register 0x93 saja
 *       0x93 &= ~0x01 ; return
 *
 * Implementasi lama kita memakai register 0x093 — itu CABANG chip 0x32,
 * BUKAN 8188E (cardType 0x0e). Akibatnya 8051 tidak pernah di-reset dengan
 * benar -> trigger offload tidak mengisi buffer -> EFUSE kosong (0xff).
 *
 * Sekarang kedua gaya (GRP A dan GRP B) didukung, dan percobaan offload
 * mencoba keduanya.
 */
static void
efuse_8051_pause_style(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int style)
{
	uint8_t v;

	if (style == RTWN_8051_STYLE_A || style == RTWN_8051_STYLE_B) {
		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x01c);
		rtwn_seq_raw_write1(rd, wr, ctx, 0x01c, (uint8_t)(v & 0xfd));

		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x01d);
		if (style == RTWN_8051_STYLE_A) {
			v = (uint8_t)(v & 0xfe);
		} else {
			v = (uint8_t)(v & 0xf7);
		}
		rtwn_seq_raw_write1(rd, wr, ctx, 0x01d, v);
	} else {
		/* cardType == 0x32 (chip lain) */
		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x093);
		rtwn_seq_raw_write1(rd, wr, ctx, 0x093, (uint8_t)(v & 0xfe));
		return;
	}

	v = rtwn_seq_raw_read1(rd, wr, ctx, 0x003);
	rtwn_seq_raw_write1(rd, wr, ctx, 0x003, (uint8_t)(v & 0xfb));
}

/* Enable 8051 — versi vendor (HAL_8051EnableIOWrapper, 0x71799). */
static void
efuse_8051_enable_style(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int style)
{
	uint8_t v;

	if (style == RTWN_8051_STYLE_A || style == RTWN_8051_STYLE_B) {
		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x01c);
		rtwn_seq_raw_write1(rd, wr, ctx, 0x01c, (uint8_t)(v & 0xfd));

		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x01d);
		if (style == RTWN_8051_STYLE_A) {
			v = (uint8_t)(v | 0x01);
		} else {
			v = (uint8_t)(v | 0x08);
		}
		rtwn_seq_raw_write1(rd, wr, ctx, 0x01d, v);
	} else {
		v = rtwn_seq_raw_read1(rd, wr, ctx, 0x093);
		rtwn_seq_raw_write1(rd, wr, ctx, 0x093, (uint8_t)(v | 0x01));
		return;
	}

	v = rtwn_seq_raw_read1(rd, wr, ctx, 0x003);
	rtwn_seq_raw_write1(rd, wr, ctx, 0x003, (uint8_t)(v | 0x04));
}

/* Wrapper untuk pengujian offline (lihat tools/test_8051.c). */
void
rtwn_8051_pause_test(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int style)
{
	efuse_8051_pause_style(rd, wr, ctx, style);
}

void
rtwn_8051_enable_test(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int style)
{
	efuse_8051_enable_style(rd, wr, ctx, style);
}

static void
efuse_8051_reset_style(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int style)
{
	efuse_8051_pause_style(rd, wr, ctx, style);
	RTWN_DELAY_MS(1);		/* vendor: stall(0x32) = 50 us */
	efuse_8051_enable_style(rd, wr, ctx, style);
}

/*
 * Baca satu blok EFUSE lewat offload.
 *
 * Mengembalikan:
 *   0  sukses
 *   -1  timeout / gagal
 *
 * Output: hdr (32 bit) + 8 byte data.
 */
static int efuse_offload_block(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t window, uint32_t *hdr, uint8_t *data8)
{
	uint8_t v;
	uint16_t i;
	uint32_t d1, d2;

	/* Mulai: set flag di 0x0F0, reset 8051, trigger (bit1 di 0x088). */
	v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL);
	rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL,
	    (uint8_t)(v | 0x80));

	efuse_8051_reset_style(rd, wr, ctx, RTWN_8051_STYLE_A);

	v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG);
	rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG,
	    (uint8_t)(v | 0x02));

	/* Tunggu sampai bit1 hilang (maks 1000 × 50 ms). */
	for (i = 0; i < 200; i++) {
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG);
		if ((v & 0x02) == 0) {
			break;
		}
		RTWN_DELAY_MS(1);
	}
	if (i >= 200) {
		/* Timeout: kembalikan flag & reset 8051. */
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL,
		    (uint8_t)(v & 0x7f));
		efuse_8051_reset_style(rd, wr, ctx, RTWN_8051_STYLE_A);
		return -1;
	}

	/* Aktifkan akses EFUSE, set window, mulai. */
	rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_ACCESS, 0x69);
	rtwn_seq_raw_write2(rd, wr, ctx, R92C_EFUSE_OFFLOAD_ADDR, window);
	rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_STAT, 0);

	/* Tunggu bit0 = siap (maks 50 × 50 ms). */
	for (i = 0; i < 30; i++) {
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_STAT);
		if (v & 0x01) {
			break;
		}
		RTWN_DELAY_MS(1);
	}
	if (i >= 30) {
		return -1;
	}

	*hdr = 0;
	rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_HDR, hdr);
	rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_DATA, &d1);
	d2 = 0;
	rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_DATA, &d2);

	/* Susun 8 byte data (little-endian, 2 x 32 bit). */
	data8[0] = (uint8_t)(d1 & 0xff);
	data8[1] = (uint8_t)((d1 >> 8) & 0xff);
	data8[2] = (uint8_t)((d1 >> 16) & 0xff);
	data8[3] = (uint8_t)((d1 >> 24) & 0xff);
	data8[4] = (uint8_t)(d2 & 0xff);
	data8[5] = (uint8_t)((d2 >> 8) & 0xff);
	data8[6] = (uint8_t)((d2 >> 16) & 0xff);
	data8[7] = (uint8_t)((d2 >> 24) & 0xff);
	return 0;
}

/*
 * Baca seluruh EFUSE 8188E USB lewat jalur offload vendor.
 *
 * Mengisi result->mac / xtal / regulatory / channel_plan.
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

int rtwn_seq_read_efuse_offload(rtwn_seq_result_t *r, rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx)
{
	uint8_t raw[256];
	uint8_t rom[512];
	uint8_t mode;
	uint16_t base;
	int attempt;

	/*
	 * ‑‑‑ DIREKONSTRUKSI LANGSUNG DARI DECOMPILATION VENDOR ‑‑‑
	 *
	 * Sumber: RtWlanU :: InitOffload_ReadEfuse_8188EU (0xb1ea5), Ghidra
	 * 12.1.4, 22 Okt 2026. Perbedaan KRITIS dengan implementasi lama:
	 *
	 *  (1) TRIGGER 8051 dilakukan SEKALI per attempt — bukan per blok!
	 *      Versi lama trigger ulang tiap blok → tiap blok dapat hasil
	 *      kosong → raw[] berisi 0xff → MAC = ff:ff:ff:ff:ff:ff.
	 *
	 *  (2) LAYOUT raw per blok (ini yang tertukar di versi lama):
	 *        raw[k*8 + 6  .. k*8 + 9]  = LE(read4(0x144))   ← header
	 *        raw[k*8 + 10 .. k*8 + 13] = LE(read4(0x148))   ← data
	 *      Versi lama menulis data (0x148) di offset +6.
	 *
	 *  (3) raw[2..5] = LE(read4(0x148)) dari blok PERTAMA.
	 *
	 *  (4) Panjang stream untuk parser = (hdr & 0xffff), bukan 256.
	 *
	 *  (5) Mode 0xaa ATAU 0xf4 (GetInitialTxBufferBoundary88E, 0xb2f7a).
	 *      Kita coba keduanya bergantian.
	 *
	 *  (6) _InitPowerOn88EU dipanggil DI DALAM fungsi ini vendor.
	 *      Ours: power-on dilakukan terpisah sebelum memanggil ini
	 *      (pakai tabel flow yang identik) → setara.
	 */
	for (attempt = 0; attempt < 8; attempt++) {
		uint32_t hdr = 0, d1 = 0, hdr2 = 0, data = 0;
		int nblocks, extra, total, k;
		uint16_t i;
		uint8_t v;
		uint16_t rawlen;
		int style;

		memset(raw, 0xff, sizeof(raw));

		/*
		 * Kombinasi yang dicoba (8 attempt):
		 *   mode 0xaa / 0xf4  ×  gaya reset 8051 GRP A / GRP B
		 * Gaya reset 8051 menentukan register 0x1d yang dipakai
		 * (lihat komentar di efuse_8051_pause_style).
		 */
		mode = (attempt % 2 == 0) ? 0xaa : 0xf4;
		style = (attempt % 4 < 2) ? RTWN_8051_STYLE_A : RTWN_8051_STYLE_B;
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_MODE, mode);
		base = (uint16_t)(mode * 0x10);

		/* ── TRIGGER (sekali per attempt) ─────────────────────────── */
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL,
		    (uint8_t)(v | 0x80));
		efuse_8051_reset_style(rd, wr, ctx, style);
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG,
		    (uint8_t)(v | 0x02));

		for (i = 0; i < 200; i++) {
			v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_TRIG);
			if ((v & 0x02) == 0) {
				break;
			}
			RTWN_DELAY_MS(1);
		}
		if (i >= 200) {
			/* Timeout: kembalikan flag & reset 8051, coba lagi. */
			v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL);
			rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL,
			    (uint8_t)(v & 0x7f));
			efuse_8051_reset_style(rd, wr, ctx, style);
			continue;
		}

		/* ── Akses EFUSE + window pertama ────────────────────────── */
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_ACCESS, 0x69);
		rtwn_seq_raw_write2(rd, wr, ctx, R92C_EFUSE_OFFLOAD_ADDR, base);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_STAT, 0);

		for (i = 0; i < 30; i++) {
			v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_STAT);
			if (v & 0x01) {
				break;
			}
			RTWN_DELAY_MS(1);
		}
		if (i >= 30) {
			continue;
		}

		rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_HDR, &hdr);
		rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_DATA, &d1);

		if ((hdr & 0xffff) >= 0x101) {
			/* Header tidak masuk akal → coba mode berikutnya. */
			continue;
		}

		/* ── Susun raw stream (LANGKA vendor) ───────────────────── */
		raw[0] = (uint8_t)(hdr >> 16);
		raw[1] = (uint8_t)(hdr >> 24);
		raw[2] = (uint8_t)(d1 & 0xff);
		raw[3] = (uint8_t)((d1 >> 8) & 0xff);
		raw[4] = (uint8_t)((d1 >> 16) & 0xff);
		raw[5] = (uint8_t)((d1 >> 24) & 0xff);

		rawlen = (uint16_t)(hdr & 0xffff);
		nblocks = (int)((rawlen - 8) >> 3);
		extra = (hdr & 7) != 0;
		total = nblocks + extra;

		for (k = 0; k < total; k++) {
			uint16_t window = (uint16_t)(base + 1 + k);

			rtwn_seq_raw_write2(rd, wr, ctx, R92C_EFUSE_OFFLOAD_ADDR,
			    window);
			rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_STAT, 0);

			for (i = 0; i < 30; i++) {
				v = rtwn_seq_raw_read1(rd, wr, ctx,
				    R92C_EFUSE_OFFLOAD_STAT);
				if (v & 0x01) {
					break;
				}
				RTWN_DELAY_MS(1);
			}
			if (i >= 30) {
				break;
			}

			rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_HDR,
			    &hdr2);
			rtwn_seq_raw_read4(rd, wr, ctx, R92C_EFUSE_OFFLOAD_DATA,
			    &data);

			/*
			 * ★ LAYOUT VENDOR:
			 *   +6  = header blok (0x144)
			 *   +10 = data blok  (0x148)
			 */
			{
				size_t o = (size_t)k * 8 + 6;
				if (o + 8 > sizeof(raw)) {
					break;
				}
				raw[o + 0] = (uint8_t)(hdr2 & 0xff);
				raw[o + 1] = (uint8_t)((hdr2 >> 8) & 0xff);
				raw[o + 2] = (uint8_t)((hdr2 >> 16) & 0xff);
				raw[o + 3] = (uint8_t)((hdr2 >> 24) & 0xff);
				raw[o + 4] = (uint8_t)(data & 0xff);
				raw[o + 5] = (uint8_t)((data >> 8) & 0xff);
				raw[o + 6] = (uint8_t)((data >> 16) & 0xff);
				raw[o + 7] = (uint8_t)((data >> 24) & 0xff);
			}
		}

		/* Ekor 0xff: dari (rawlen-1) s/d rawlen + (hdr&7) + 5. */
		{
			uint32_t tail = (uint32_t)rawlen + (hdr & 7) + 5;
			uint32_t j;
			for (j = (rawlen > 1) ? (rawlen - 1) : 0; j < tail && j < sizeof(raw); j++) {
				raw[j] = 0xff;
			}
		}

		/* ── Parse (panjang stream = hdr & 0xffff, bukan 256) ───── */
		memset(rom, 0xff, sizeof(rom));
		rtwn_seq_efuse_parse(raw, rawlen, rom);

		/*
		 * DIAGNOSTIK (22 Okt 2026): kenapa offload ditolak?
		 */
		{
			char dbg[48];
			uint8_t meta[8];

			meta[0] = mode;
			meta[1] = (uint8_t)(hdr >> 24);
			meta[2] = (uint8_t)(hdr >> 16);
			meta[3] = (uint8_t)(hdr >> 8);
			meta[4] = (uint8_t)hdr;
			meta[5] = (uint8_t)(rawlen >> 8);
			meta[6] = (uint8_t)rawlen;
			meta[7] = 0;
			m1_hex(dbg, sizeof(dbg), meta, 7);
			rtwn_seq_diag_set(ctx, "efuse-offload-hdr", dbg);
			m1_hex(dbg, sizeof(dbg), raw, 8);
			rtwn_seq_diag_set(ctx, "efuse-offload-raw", dbg);
			{
				/*
				 * Dump lengkap (untuk analisis parser offline).
				 * 256 byte = 513 karakter.
				 */
				char big[560];
				uint16_t lim = (rawlen > 256) ? 256 : rawlen;

				m1_hex(big, sizeof(big), raw, lim);
				rtwn_seq_diag_set(ctx, "efuse-dump", big);
			}
			{
				uint8_t r[5];

				r[0] = rom[0];
				r[1] = rom[0xD0];
				r[2] = rom[0xD1];
				r[3] = rom[0xD2];
				r[4] = rom[0xD3];
				m1_hex(dbg, sizeof(dbg), r, 5);
				rtwn_seq_diag_set(ctx, "efuse-offload-rom", dbg);
			}
		}

		memcpy(r->mac, rom + 215, 6);
		r->xtal = rom[185];
		r->channel_plan = rom[184];
		r->regulatory = (uint8_t)(rom[193] & 0x07);
		r->efuse_ok = 1;
		r->step = 2;

		/* Bersihkan: kembalikan flag & reset 8051. */
		v = rtwn_seq_raw_read1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_EFUSE_OFFLOAD_CTRL,
		    (uint8_t)(v & 0x7f));
		efuse_8051_reset_style(rd, wr, ctx, RTWN_8051_STYLE_A);

		/*
		 * Validasi hasil (dari decompilation vendor):
		 *   if (*out_rom == ')')            → byte pertama image = 0x29
		 *   dan sc[199] == 0x81
		 * Kalau byte pertama bukan 0x29 → image EFUSE tidak valid
		 * → coba mode berikutnya (0xaa / 0xf4).
		 *
		 * Ini MUCH lebih kuat daripada mengecek MAC != 0xff:
		 * MAC bisa saja valid-looking walau segmentnya salah.
		 */
		/*
		 * Validasi #2 (lebih kuat): image EFUSE 8188E menyimpan USB
		 * VID/PID di 0xD0/0xD2 (lihat `struct r88e_rom` FreeBSD).
		 * Harus 2357:010c untuk chip kita.
		 */
		if (rom[0] == 0x29 &&
		    rom[0xD0] == 0x57 && rom[0xD1] == 0x23 &&
		    rom[0xD2] == 0x0c && rom[0xD3] == 0x01 &&
		    r->mac[0] != 0xff && r->mac[0] != 0x00) {
			return 0;
		}
		memset(r->mac, 0, sizeof(r->mac));
		r->efuse_ok = 0;
	}

	return -1;
}
