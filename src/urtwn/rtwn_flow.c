/*
 * rtwn_flow.c — eksekutor "power flow" gaya driver vendor Realtek.
 *
 * Driver vendor Realtek mendeskripsikan urutan inisialisasi chip sebagai tabel
 * data statis (`_rtl8188E_power_on_flow` dst). Tabel ini dibaca oleh
 * `HalPwrSeqCmdParsing` yang mengeksekusi tiap entri.
 *
 * Format entri (WL_PWR_CFG, 8 byte):
 *   +0  u16  Register
 *   +2  u8   BitMaskReg    (entri dilewati kalau 0)
 *   +3  u16  Ext           (unaligned)
 *          Ext & 0x0f        = unit mask
 *          (Ext >> 4) & 0x0f = type mask
 *          Ext >> 12         = tipe perintah
 *   +5  u8   AndMask
 *   +6  u8   OrMask
 *
 * Tipe perintah:
 *   1 WRITE : reg = (reg & ~AndMask) | (OrMask & AndMask)
 *   2 POLL  : tunggu sampai (reg & AndMask) == (OrMask & AndMask)
 *   3 DELAY : tunggu reg ms (reg*1000 ms kalau OrMask != 0)
 *   4 END   : selesai
 *
 * Tabel di bawah diekstrak dari binary vendor RtWlanU.kext
 * (lihat tools/decode_flow_table.py).
 */

#include "rtwn_seq.h"
#include "rtwn_platform.h"
#include <string.h>


/* Tipe perintah. */
#define RTWN_FLOW_WRITE 1
#define RTWN_FLOW_POLL  2
#define RTWN_FLOW_DELAY 3
#define RTWN_FLOW_END   4

typedef struct {
	uint16_t reg;
	uint8_t  bitmask;
	uint16_t ext;
	uint8_t  andmask;
	uint8_t  ormask;
} rtwn_flow_entry_t;

/* --- Register 8188E --- */
#define R88E_SYS_CLKR      0x006
#define R88E_SYS_FUNC_EN   0x002
#define R88E_AFE_XTAL      0x026
#define R88E_LPLDO_CTRL    0x023
#define R88E_EFUSE_MODE    0x0f2
#define R88E_HWPDN         0x074
#define R88E_HWPDN_CTRL    0x005

/* _rtl8188E_power_on_flow (vendor, @0x2b6240) */
static const rtwn_flow_entry_t flow_8188e_power_on[] = {
	{ 0x0006, 0xff, 0x20ff, 0x02, 0x02 },	/* POLL  clkr bit1 SET   */
	{ 0x00f2, 0xff, 0x10ff, 0x10, 0x00 },	/* WRITE efuse mode     */
	{ 0x0002, 0xff, 0x10ff, 0x03, 0x00 },	/* WRITE sys func en    */
	{ 0x0026, 0xff, 0x10ff, 0x80, 0x80 },	/* WRITE xtal ctrl      */
	{ 0x0005, 0xff, 0x10ff, 0x80, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x18, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x01, 0x01 },
	{ 0x0005, 0xff, 0x20ff, 0x01, 0x00 },	/* POLL  bit0 CLEAR     */
	{ 0x0023, 0xff, 0x10ff, 0x10, 0x00 },	/* WRITE lpldo ctrl     */
	{ 0x0074, 0xff, 0x101f, 0x10, 0x10 },	/* WRITE hwpdn          */
	{ 0xffff, 0xff, 0x40ff, 0x00, 0x00 }	/* END */
};

/* _rtl8188E_hwpdn_flow (vendor, @0x2b65c0) */
static const rtwn_flow_entry_t flow_8188e_hwpdn[] = {
	{ 0x001f, 0xff, 0x10ff, 0xff, 0x00 },
	{ 0x0023, 0xff, 0x10ff, 0x10, 0x10 },
	{ 0x0005, 0xff, 0x10ff, 0x02, 0x02 },
	{ 0x0005, 0xff, 0x20ff, 0x02, 0x00 },
	{ 0x0006, 0xff, 0x10ff, 0x01, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x80, 0x80 },
	{ 0xffff, 0xff, 0x40ff, 0x00, 0x00 }
};

/* _rtl8188E_radio_off_flow (vendor, @0x2b62a0) */
static const rtwn_flow_entry_t flow_8188e_radio_off[] = {
	{ 0x001f, 0xff, 0x10ff, 0xff, 0x00 },
	{ 0x0026, 0xff, 0x10ff, 0x80, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x02, 0x00 },
	{ 0x0006, 0xff, 0x10ff, 0x01, 0x00 },
	{ 0x0002, 0xff, 0x10ff, 0x02, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x02, 0x02 },
	{ 0x0005, 0xff, 0x20ff, 0x02, 0x00 },
	{ 0x0020, 0xff, 0x10ff, 0x08, 0x00 },
	{ 0x0000, 0xff, 0x103f, 0x20, 0x20 },
	{ 0xffff, 0xff, 0x40ff, 0x00, 0x00 }
};

/* _rtl8188E_card_enable_flow (vendor, @0x2b63b0) */
static const rtwn_flow_entry_t flow_8188e_card_enable[] = {
	{ 0x0005, 0xff, 0x10ff, 0x80, 0x00 },
	{ 0x0005, 0xff, 0x10ff, 0x02, 0x00 },
	{ 0x0005, 0xff, 0x20ff, 0x02, 0x02 },
	{ 0x0006, 0xff, 0x10ff, 0x01, 0x00 },
	{ 0x0002, 0xff, 0x10ff, 0x02, 0x00 },
	{ 0x0020, 0xff, 0x10ff, 0x08, 0x00 },
	{ 0x0000, 0xff, 0x103f, 0x20, 0x20 },
	{ 0x0007, 0xff, 0x101f, 0xff, 0x20 },
	{ 0x0005, 0xff, 0x103f, 0x18, 0x08 },
	{ 0x0005, 0xff, 0x104f, 0x04, 0x04 },
	{ 0x0005, 0xff, 0x10ff, 0x02, 0x02 },
	{ 0x0005, 0xff, 0x20ff, 0x02, 0x00 },
	{ 0x0006, 0xff, 0x10ff, 0x01, 0x00 },
	{ 0x0002, 0xff, 0x10ff, 0x02, 0x00 },
	{ 0x0023, 0xff, 0x10ff, 0x10, 0x00 },
	{ 0x0074, 0xff, 0x101f, 0x10, 0x00 },
	{ 0xffff, 0xff, 0x40ff, 0x00, 0x00 }
};

/*
 * Jamin setiap tabel berakhir dengan entri END.
 *
 * Kalau tabel rusak (entri END hilang), rtwn_run_flow() kini aman
 * (batas jumlah entri) — tapi lebih baik gagal saat compile.
 */
_Static_assert(sizeof(flow_8188e_power_on) / sizeof(flow_8188e_power_on[0]) == 11,
    "flow_8188e_power_on berubah jumlah entri");
_Static_assert(sizeof(flow_8188e_hwpdn) / sizeof(flow_8188e_hwpdn[0]) == 7,
    "flow_8188e_hwpdn berubah jumlah entri");
_Static_assert(sizeof(flow_8188e_radio_off) / sizeof(flow_8188e_radio_off[0]) == 10,
    "flow_8188e_radio_off berubah jumlah entri");
_Static_assert(sizeof(flow_8188e_card_enable) / sizeof(flow_8188e_card_enable[0]) == 17,
    "flow_8188e_card_enable berubah jumlah entri");

/*
 * Jalankan satu flow.
 * Return 0 = sukses, -1 = timeout pada POLL.
 */
static int rtwn_run_flow(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    const rtwn_flow_entry_t *flow, uint32_t nentries, uint32_t poll_timeout_ms)
{
	uint32_t waited = 0;
	uint32_t i;

	/*
	 * PERBAIKAN KEAMANAN (audit 22 Okt 2026).
	 *
	 * Sebelumnya loop ini `for (;;)` dan hanya berhenti kalau
	 * menemukan entri END. Kalau ada tabel yang salah edit (entri END
	 * hilang), pointer `flow` akan berjalan keluar dari array → OOB
	 * baca .rodata → isi sampah ditafsirkan sebagai perintah WRITE
	 * ke register sembarang.
	 *
	 * Sekarang jumlah entri dibatasi eksplisit.
	 */
	if (!flow || nentries == 0) {
		return -1;
	}

	for (i = 0; i < nentries; i++, flow++) {
		uint16_t reg = flow->reg;
		uint8_t cmd = (uint8_t)(flow->ext >> 12);
		uint8_t andm = flow->andmask;
		uint8_t orm = flow->ormask;

		if (cmd == RTWN_FLOW_END) {
			return 0;
		}

		/* Entri dilewati kalau BitMaskReg == 0 atau type/unit mask 0. */
		if (flow->bitmask == 0 || (flow->ext & 0x0f) == 0 ||
		    ((flow->ext >> 4) & 0x0f) == 0) {
			continue;
		}

		switch (cmd) {
		case RTWN_FLOW_WRITE: {
			uint8_t v = rtwn_seq_raw_read1(rd, wr, ctx, reg);
			uint8_t nv = (uint8_t)((~andm & v) | (orm & andm));
			rtwn_seq_raw_write1(rd, wr, ctx, reg, nv);
			break;
		}
		case RTWN_FLOW_POLL: {
			uint8_t want = (uint8_t)(orm & andm);
			for (;;) {
				uint8_t v = rtwn_seq_raw_read1(rd, wr, ctx, reg);
				if ((uint8_t)(v & andm) == want) {
					break;
				}
				if (waited >= poll_timeout_ms) {
					return -1;
				}
				RTWN_DELAY_MS(10);
				waited += 10;
			}
			break;
		}
		case RTWN_FLOW_DELAY: {
			uint32_t ms = reg;
			if (orm != 0) {
				ms *= 1000;
			}
			/*
			 * Batas keras (audit 22 Okt 2026): tanpa clamp,
			 * satu entri DELAY bisatidur 65.535.000 ms ≈ 18 jam
			 * di dalam kernel. Tidak ada tabel yang memakai
			 * DELAY saat ini, tapi jangan pernah biarkan
			 * kernel bisa tergolong tidurkan半天.
			 */
			if (ms > 1000) {
				ms = 1000;
			}
			RTWN_DELAY_MS(ms);
			break;
		}
		default:
			break;
		}
	}

	/* Jalur habis tanpa entri END → error (tabel rusak). */
	return -1;
}

int rtwn_flow_power_on_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	return rtwn_run_flow(rd, wr, ctx, flow_8188e_power_on, 11, 5000);
}

int rtwn_flow_hwpdn_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	return rtwn_run_flow(rd, wr, ctx, flow_8188e_hwpdn, 7, 5000);
}

int rtwn_flow_radio_off_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	return rtwn_run_flow(rd, wr, ctx, flow_8188e_radio_off, 10, 5000);
}

int rtwn_flow_card_enable_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	return rtwn_run_flow(rd, wr, ctx, flow_8188e_card_enable, 17, 5000);
}

/*
 * Init LLT table 8188E — versi vendor (InitLLTTable8188E @0xae125).
 *
 * Berbeda dari OpenBSD (urtwn_llt_init): vendor membangun pemetaan
 * "halaman → halaman berikutnya" dengan 2 register:
 *   0x1E0 = (addr & 0xFF) | ((next & 0xFF) << 8) | 0x40000000 (op WRITE)
 * dan menutup daftar dengan 0x400000FF.
 *
 * page_count = jumlah halaman TX (OpenBSD: R88E_TX_PAGE_COUNT = 168).
 * total      = 0xAF (175) atau 0xFF (255) untuk tipe chip tertentu.
 */
int rtwn_llt_init_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint32_t page_count)
{
	uint32_t total = 0xaf;		/* 175 */
	uint32_t i, j;

	for (i = 1; i < page_count; i++) {
		uint32_t v = (i & 0xff) | ((i - 1) << 8) | 0x40000000;
		rtwn_seq_raw_write4(rd, wr, ctx, 0x1e0, v);
		(void)rtwn_seq_raw_read4(rd, wr, ctx, 0x1e0, &v);
	}
	{
		uint32_t v = ((page_count - 1) & 0xff) << 8 | 0x400000ff;
		rtwn_seq_raw_write4(rd, wr, ctx, 0x1e0, v);
		(void)rtwn_seq_raw_read4(rd, wr, ctx, 0x1e0, &v);
	}

	/* Sisa halaman (page_count..total-1) → rantai melingkar i -> i+1 (InitLLTTable8188E @0xae1ee). */
	for (i = page_count, j = page_count << 8; i < total; i++, j += 0x100) {
		uint32_t v = ((i + 1) & 0xff) | (j & 0xff00) | 0x40000000;
		rtwn_seq_raw_write4(rd, wr, ctx, 0x1e0, v);
		(void)rtwn_seq_raw_read4(rd, wr, ctx, 0x1e0, &v);
	}
	{
		uint32_t v = (page_count & 0xff) | (total << 8) | 0x40000000;
		rtwn_seq_raw_write4(rd, wr, ctx, 0x1e0, v);
		(void)rtwn_seq_raw_read4(rd, wr, ctx, 0x1e0, &v);
	}
	return 0;
}
