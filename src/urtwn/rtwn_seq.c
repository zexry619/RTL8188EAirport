/*
 * rtwn_seq.c — urutan M1 (portabel, tanpa IOKit).
 *
 * Diport dari OpenBSD (ISC):
 *   sys/dev/ic/rtwn.c        : rtwn_efuse_read, rtwn_efuse_switch_power,
 *                               rtwn_r88e_read_rom, rtwn_load_firmware
 *   sys/dev/usb/if_urtwn.c   : urtwn_r88e_power_on, urtwn_dma_init,
 *                               urtwn_llt_init, urtwn_llt_write,
 *                               urtwn_fw_loadpage
 *
 * Modifikasi agar portabel:
 *   - I/O lewat callback (bukan usbd_do_request)
 *   - tanpa mbuf/net80211/ifnet
 *   - delay dalam milidetik (OpenBSD DELAY = mikrodetik; di sini IODelay/Sleep)
 *
 * Lisensi: GPL-2.0-or-later (proyek), turunan ISC.
 */

#include "rtwn_seq.h"
#include "rtwn_platform.h"
#include <string.h>

/* --- Register & bit yang dipakai (dari r92creg.h OpenBSD) --- */
#define R92C_SYS_ISO_CTRL            0x000
#define R92C_SYS_FUNC_EN             0x002
#define R92C_APS_FSMCO               0x004
#define R92C_SYS_CLKR                0x008
#define R92C_EFUSE_CTRL              0x030
#define R92C_MCUFWDL                 0x080
#define R92C_SYS_CFG                 0x0f0
#define R92C_CR                      0x100
#define R92C_EFUSE_ACCESS            0x0cf
#define R92C_MACID                   0x610

#define R92C_SYS_ISO_CTRL_PWC_EV12V  0x8000
#define R92C_SYS_FUNC_EN_ELDR        0x1000
#define R92C_SYS_FUNC_EN_CPUEN       0x0400
#define R92C_SYS_CLKR_LOADER_EN      0x0020
#define R92C_SYS_CLKR_ANA8M          0x0002

#define R92C_APS_FSMCO_APFM_ONMAC    0x00000100
#define R92C_APS_FSMCO_AFSM_HSUS    0x00000800
#define R92C_APS_FSMCO_AFSM_PCIE    0x00001000
#define R92C_APS_FSMCO_APDM_HPDN    0x00008000
#define R92C_APS_FSMCO_SUS_HOST     0x00020000

#define R92C_CR_HCI_TXDMA_EN         0x00000001
#define R92C_CR_HCI_RXDMA_EN         0x00000002
#define R92C_CR_TXDMA_EN             0x00000004
#define R92C_CR_RXDMA_EN             0x00000008
#define R92C_CR_PROTOCOL_EN          0x00000010
#define R92C_CR_SCHEDULE_EN          0x00000020
#define R92C_CR_ENSEC                0x00000200
#define R92C_CR_CALTMR_EN            0x00000400

#define R92C_EFUSE_CTRL_DATA_M       0x000000ff
#define R92C_EFUSE_CTRL_DATA_S       0
#define R92C_EFUSE_CTRL_ADDR_M       0x0003ff00
#define R92C_EFUSE_CTRL_ADDR_S       8
#define R92C_EFUSE_CTRL_VALID        0x80000000
#define R92C_EFUSE_ACCESS_ON         0x69
#define R92C_EFUSE_ACCESS_OFF        0x00

#define R92C_MCUFWDL_EN              0x00000001
#define R92C_MCUFWDL_RDY             0x00000002
#define R92C_MCUFWDL_CHKSUM_RPT      0x00000004
#define R92C_MCUFWDL_RAM_DL_SEL      0x00000080
#define R92C_MCUFWDL_PAGE_M          0x00070000
#define R92C_MCUFWDL_PAGE_S          16
#define R92C_MCUFWDL_ROM_DLEN        0x00080000
#define R92C_MCUFWDL_WINTINI_RDY     0x00000040

#define R92C_FW_START_ADDR           0x1000
#define R92C_FW_PAGE_SIZE            4096

#define R92C_RSV_CTRL                0x01c
#define R92C_RSV_CTRL_WLOCK_00       0x0001
#define R88E_RSV_CTRL_MCU_RST        0x0800

/* Konstanta halaman/queue 88E (RtWlanU GetInitialRQPValue88EU: HPQ=12, NPQ=13, PUBQ=144, boundary=170). */
#define R88E_HQ_NPAGES               12
#define R88E_LQ_NPAGES               0
#define R88E_NQ_NPAGES               13
#define R88E_TX_PAGE_COUNT           169

/* Callback delay (ms). Default: tidak.acak. */

/* --- helper MS/SM/RW (OpenBSD r92creg.h) --- */
/*
 * Stub lemah: dipakai hanya bila rtwn_flow.c tidak ikut di-link
 * (mis. unit test mandiri). Implementasi asli ada di rtwn_flow.c.
 */
#if defined(__APPLE__) || defined(__GNUC__)
__attribute__((weak)) int rtwn_flow_power_on_8188e(rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx)
{
	(void)rd; (void)wr; (void)ctx;
	return -1;
}
__attribute__((weak)) int rtwn_llt_init_8188e(rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx, uint32_t page_count)
{
	(void)rd; (void)wr; (void)ctx; (void)page_count;
	return -1;
}
#endif

/* Log portable (diarahkan ke callback pengguna; default = diam). */
#ifndef RTWN_LOG
#define RTWN_LOG(msg) ((void)(msg))
#endif
#define IOLog_SEQ(msg) RTWN_LOG(msg)

#define MS(val, field) (((val) & field##_M) >> field##_S)
#define SM(field, val) (((val) << field##_S) & field##_M)
#define RW(var, field, val) (((var) & ~field##_M) | SM(field, val))

/* --- akses register --- */
static uint8_t rd8(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a)
{
	uint8_t v = 0xff;
	rd(ctx, a, &v, 1);
	return v;
}

static uint16_t rd16(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a)
{
	uint8_t b[2] = { 0xff, 0xff };
	rd(ctx, a, b, 2);
	return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t rd32(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a)
{
	uint8_t b[4] = { 0xff, 0xff, 0xff, 0xff };
	rd(ctx, a, b, 4);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void wr8(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a,
    uint8_t v)
{
	wr(ctx, a, &v, 1);
}

static void wr16(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a,
    uint16_t v)
{
	uint8_t b[2];
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)(v >> 8);
	wr(ctx, a, b, 2);
}

static void wr32(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t a,
    uint32_t v)
{
	uint8_t b[4];
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)((v >> 8) & 0xff);
	b[2] = (uint8_t)((v >> 16) & 0xff);
	b[3] = (uint8_t)((v >> 24) & 0xff);
	wr(ctx, a, b, 4);
}


/* ── Hook diagnostik (lihat rtwn_seq.h) ──────────────────────────── */

static rtwn_diag_fn g_rtwn_diag_fn = NULL;

void
rtwn_seq_diag_register(rtwn_diag_fn fn)
{
	g_rtwn_diag_fn = fn;
}

void
rtwn_seq_diag_set(void *ctx, const char *key, const char *val)
{
	if (g_rtwn_diag_fn && key && val) {
		g_rtwn_diag_fn(ctx, key, val);
	}
}


/* ------------------------------------------------------------------ */
/* 1. Baca register dasar                                              */
/* ------------------------------------------------------------------ */
int rtwn_seq_read_regs(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx)
{
	r->cr = rd32(rd, wr, ctx, R92C_CR);
	r->mcuwdl = rd32(rd, wr, ctx, R92C_MCUFWDL);
	r->syscfg = rd32(rd, wr, ctx, R92C_SYS_CFG);
	r->chip_ok = 1;
	r->step = 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 2. EFUSE (OpenBSD: rtwn_efuse_read)                                 */
/* ------------------------------------------------------------------ */
/*
 * Baca satu byte EFUSE.
 *
 * ‑‑‑ PERBAIKAN (audit 22 Okt 2026) ‑‑‑
 *
 * Versi lama mengembalikan `uint8_t` dan memakai 0xff sebagai penanda
 * timeout. Padahal 0xff juga nilai LEGAL untuk sel EFUSE yang belum
 * diprogram → timeout (chip mati) tidak bisa dibedakan dari data →
 * hasil: EFUSE "OK" dengan MAC FF:FF:FF:FF:FF:FF.
 *
 * Sekarang: return 0 = sukses (nilai di *out), -1 = timeout.
 */
static int efuse_read_1(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t addr, uint8_t *out)
{
	uint32_t reg;
	int ntries;

	reg = rd32(rd, wr, ctx, R92C_EFUSE_CTRL);
	reg = RW(reg, R92C_EFUSE_CTRL_ADDR, addr);
	reg &= ~R92C_EFUSE_CTRL_VALID;
	wr32(rd, wr, ctx, R92C_EFUSE_CTRL, reg);

	for (ntries = 0; ntries < 100; ntries++) {
		reg = rd32(rd, wr, ctx, R92C_EFUSE_CTRL);
		if (reg & R92C_EFUSE_CTRL_VALID) {
			*out = (uint8_t)MS(reg, R92C_EFUSE_CTRL_DATA);
			return 0;
		}
		RTWN_DELAY_MS(1);
	}
	return -1;	/* timeout — chip tidak merespons */
}

/* OpenBSD: rtwn_efuse_switch_power() */
static void efuse_switch_power(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	uint16_t reg;

	reg = rd16(rd, wr, ctx, R92C_SYS_ISO_CTRL);
	if (!(reg & R92C_SYS_ISO_CTRL_PWC_EV12V)) {
		wr16(rd, wr, ctx, R92C_SYS_ISO_CTRL,
		    reg | R92C_SYS_ISO_CTRL_PWC_EV12V);
	}
	reg = rd16(rd, wr, ctx, R92C_SYS_FUNC_EN);
	if (!(reg & R92C_SYS_FUNC_EN_ELDR)) {
		wr16(rd, wr, ctx, R92C_SYS_FUNC_EN, reg | R92C_SYS_FUNC_EN_ELDR);
	}
	reg = rd16(rd, wr, ctx, R92C_SYS_CLKR);
	if ((reg & (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) !=
	    (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) {
		wr16(rd, wr, ctx, R92C_SYS_CLKR,
		    reg | R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M);
	}
}

/*
 * Baca EFUSE 8188E.
 *
 * Algoritme mengikuti decompilation driver vendor
 * (RtWlanU::ParsingEfuseContent_8188EU) — BUKAN OpenBSD rtwn_efuse_read.
 * Perbedaan penting:
 *
 *   OpenBSD : rom[off * 8 + i * 2 + 0/1]          (off = nibble tinggi, 0..0x3F)
 *   Vendor  : dst[(off & 0x3F) * 8 + i * 2 + 0/1] (off = hasil parse header)
 *
 * Vendor juga mem-broadcast setiap byte 2× (2 bank) lalu menyalin hasilnya
 * dengan stride 8 byte per blok 4 byte.
 *
 * Paramater:
 *   param_2 = buffer EFUSE mentah (yang dibaca dari chip)
 *   param_3 = buffer output (512 byte)
 *   param_4 = panjang output yang diminta
 *   param_5 = panjang buffer mentah
 *   param_6 = offset output
 */
/*
 * Parse konten EFUSE 8188E.
 *
 * Algoritme disalin dari decompilation driver vendor
 * (RtWlanU::ParsingEfuseContent_8188EU) dengan notasi yang sama:
 *
 *   raw[]      = byte EFUSE mentah yang dibaca dari chip
 *   block[]    = 64 blok x 4 uint16  (index: blok*4 + slot)
 *   out[]      = 512 byte; blok N occupies out[N*8 .. N*8+7]
 *
 * Loop utama (dari vendor):
 *   while (cur != 0xff && ri < rawlen):
 *       if ((cur & 0x1f) == 0x0f):            # header ekstensi
 *           nxt = raw[ri]
 *           if ((nxt & 0x0f) != 0x0f):
 *               off = ((nxt >> 1) & 0x78) | (cur >> 5)
 *           else: skip 2 byte
 *       else:
 *           off = cur >> 4
 *           mask = cur & 0x0f
 *       for i in 0..3:
 *           if !(mask & (1<<i)):
 *               v = raw[ri] | (raw[ri+1] << 8)   # 2 byte little-endian
 *               block[(off & 0x3f)*4 + i] = v
 *               ri += 2
 */
static void efuse_parse_8188e(const uint8_t *raw, uint16_t rawlen,
    uint8_t *out)
{
	uint16_t block[256];
	int i, j;
	size_t ri;
	uint8_t cur;

	for (i = 0; i < 256; i++)
		block[i] = 0xffff;
	memset(out, 0xff, 512);

	if (rawlen < 2)
		return;

	cur = raw[0];
	ri = 1;

	while (cur != 0xff && ri < (size_t)rawlen) {
		uint8_t off, mask;

		if ((cur & 0x1f) == 0x0f) {
			/* Header ekstensi 2 byte (blok >= 16). */
			uint8_t nxt = (ri < (size_t)rawlen) ? raw[ri] : 0xff;

			mask = (uint8_t)(nxt & 0x0f);
			if (mask != 0x0f) {
				off = (uint8_t)(((nxt >> 1) & 0x78) |
				    (cur >> 5));
				ri++;
			} else {
				ri += 2;
				cur = (ri < (size_t)rawlen) ? raw[ri] : 0xff;
				continue;
			}
		} else {
			/* Header 1 byte: nibble atas = blok, bawah = mask. */
			off = (uint8_t)(cur >> 4);
			mask = (uint8_t)(cur & 0x0f);
		}

		/*
		 * ── SEMANTIK MASK (diverifikasi pada dump EFUSE nyata) ──
		 *
		 *   bit i = 1 → word i ABSENT  (0 byte, tidak ada data)
		 *   bit i = 0 → word i PRESENT (2 byte, little-endian)
		 *
		 * Bukti (dump 8188E, 256 byte):
		 *   0e | 29 81                    → mask 0x0e = 1 word
		 *   4f 30 | 57 23 0c 01 43 66 00 98  → mask 0x00 = 4 word
		 *   2f 3e | 00 03                 → mask 0x0e = 1 word
		 *   af 30 | 38 30 32 2e 31 31 6e 20  → "802.11n "
		 *
		 * Implementasi lama keliru: untuk bit SET ia tetap memajukan
		 * `ri` sebesar 1 byte. Akibatnya seluruh stream bergeser dan
		 * MAC tidak pernah ditemukan (rom[0] != 0x29).
		 */
		for (i = 0; i < 4; i++) {
			if ((mask >> i) & 1)
				continue;	/* word absent */
			if (ri + 1 >= (size_t)rawlen)
				break;
			block[((off & 0x3f) * 4) + i] =
			    (uint16_t)(raw[ri] |
			    ((uint16_t)raw[ri + 1] << 8));
			ri += 2;
		}

		cur = (ri < (size_t)rawlen) ? raw[ri] : 0xff;
		if (cur != 0xff)
			ri++;
	}

	/* Susun output: blok N -> out[N*8 .. N*8+7], little-endian. */
	for (i = 0; i < 64; i++) {
		for (j = 0; j < 4; j++) {
			uint16_t v = block[i * 4 + j];

			out[i * 8 + j * 2 + 0] = (uint8_t)(v & 0xff);
			out[i * 8 + j * 2 + 1] = (uint8_t)(v >> 8);
		}
	}
}

/* Helper level rendah — diekspos untuk rtwn_seq_efuse.c & test. */
uint8_t rtwn_seq_raw_read1(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t addr)
{
	return rd8(rd, wr, ctx, addr);
}

void rtwn_seq_raw_write1(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint8_t val)
{
	wr8(rd, wr, ctx, addr, val);
}

void rtwn_seq_raw_read4(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint32_t *out)
{
	*out = rd32(rd, wr, ctx, addr);
}

void rtwn_seq_raw_write2(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint16_t val)
{
	wr16(rd, wr, ctx, addr, val);
}

void rtwn_seq_raw_write4(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint32_t val)
{
	wr32(rd, wr, ctx, addr, val);
}

/* Parser (identik vendor — divalidasi differential test). */
void rtwn_seq_efuse_parse(const uint8_t *raw, uint16_t rawlen, uint8_t *out)
{
	efuse_parse_8188e(raw, rawlen, out);
}

/* Diekspos untuk differential test (tools/test_efuse_diff.c). */
void rtwn_seq_parse_efuse_for_test(const uint8_t *raw, uint16_t rawlen,
    uint8_t *out)
{
	efuse_parse_8188e(raw, rawlen, out);
}

int rtwn_seq_read_efuse(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx)
{
	uint8_t raw[256];
	uint8_t rom[512];
	uint32_t addr;
	const uint32_t len = 256;	/* 88E */
	int timedOut = 0;

	/* 88E: aktifkan akses EFUSE */
	wr8(rd, wr, ctx, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_ON);
	efuse_switch_power(rd, wr, ctx);

	/*
	 * Akuisisi: baca byte EFUSE mentah SEQUENTIAL (0..255) tanpa
	 * interpretasi apa pun. Parsing dilakukan terpisah oleh
	 * efuse_parse_8188e() — sama seperti driver vendor
	 * (ParsingEfuseContent_8188EU menerima buffer mentah).
	 *
	 * PERBAIKAN (audit 22 Okt 2026):
	 *   - batas loop dulu `(uint16_t)len` → nilainya 0 (256 dipotong
	 *     jadi 0) sehingga loop TIDAK PERNAH jalan → raw[] tetap
	 *     0xff semua → EFUSE "OK" dengan MAC FF:FF:FF:FF:FF:FF.
	 *   - timeout chip kini dibedakan dari nilai 0xff yang sah.
	 */
	for (addr = 0; addr < len && addr < sizeof(raw); addr++) {
		uint8_t v = 0xff;

		if (efuse_read_1(rd, wr, ctx, (uint16_t)addr, &v) != 0) {
			/* Chip tidak merespons → ini ERROR, bukan "akhir map". */
			timedOut = 1;
			break;
		}
		/*
		 * JANGAN berhenti di byte 0xff pertama!
		 *
		 * Temuan riset (FreeBSD sys/dev/rtwn, 22 Okt 2026): pembaca
		 * EFUSE yang benar bersifat HEADER-DRIVEN, bukan "berhenti
		 * di 0xff":
		 *
		 *   loop:
		 *     hdr = byte(addr)
		 *     if (hdr == 0xff) break        ← 0xff HANYA berarti
		 *     off = hdr >> 4;                "akhir map" kalau
		 *     msk = hdr & 0x0f               yang dibaca adalah
		 *     words = popcount(~msk & 0x0f)  HEADER
		 *     baca words × 2 byte data
		 *     addr += 1 + words*2
		 *
		 * Byte 0xff bisa muncul DI TENGAH blok data (sel EFUSE yang
		 * belum diprogram) → berhenti di sana memotong map.
		 *
		 * Perbaikan: baca SELURUH 256 byte fisiknya; parser
		 * (efuse_parse_8188e) yangahi tahu kapan map berakhir
		 * (karena ia menghitung jumlah byte data dari mask).
		 */
		raw[addr] = v;
	}
	if (addr < sizeof(raw)) {
		memset(raw + addr, 0xff, sizeof(raw) - addr);
	}

	/* Matikan lagi akses EFUSE */
	wr8(rd, wr, ctx, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_OFF);

	if (timedOut) {
		/* Jangan laporkan sukses — chip tidak hidup. */
		return -1;
	}

	/* Parse dengan algoritme vendor. */
	memset(rom, 0xff, sizeof(rom));
	efuse_parse_8188e(raw, (uint16_t)len, rom);

	/*
	 * Offset MAC di image ROM 88E (USB) = 215.
	 * Lihat struct r88e_rom → union u.r88eu → macaddr.
	 */
	memcpy(r->mac, rom + 215, 6);
	r->xtal = rom[185];
	r->channel_plan = rom[184];
	r->regulatory = (uint8_t)(rom[193] & 0x07);

	/*
	 * Sanity check MAC (audit 22 Okt 2026).
	 *
	 * MAC FF:FF:FF:FF:FF:FF / 00:00:00:00:00:00 / multicast berarti
	 * EFUSE TIDAK terbaca — jangan sampai dilaporkan "OK".
	 */
	{
		int i, allFF = 1, allZero = 1;
		for (i = 0; i < 6; i++) {
			if (r->mac[i] != 0xff) allFF = 0;
			if (r->mac[i] != 0x00) allZero = 0;
		}
		if (allFF || allZero || (r->mac[0] & 0x01)) {
			memset(r->mac, 0, sizeof(r->mac));
			r->efuse_ok = 0;
			r->step = 2;
			return -1;
		}
	}

	r->efuse_ok = 1;
	r->step = 2;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 3. Power on (OpenBSD: urtwn_r88e_power_on)                          */
/* ------------------------------------------------------------------ */
/*
 * Power-on 8188E.
 *
 * Mengikuti tabel statik vendor `_rtl8188E_power_on_flow` (lihat rtwn_flow.c)
 * — BUKAN urutan OpenBSD, karena urutan vendor berinteraksi dengan 8051
 * (ada entri POLL yang menunggu bit status 8051).
 */
int rtwn_seq_power_on(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx)
{
	if (rtwn_flow_power_on_8188e(rd, wr, ctx) != 0) {
		IOLog_SEQ("power on: timeout pada flow vendor\n");
		return -1;
	}

	/* Aktifkan blok MAC DMA/WMAC/SCHEDULE/SEC (OpenBSD). */
	{
		uint32_t reg = rd16(rd, wr, ctx, R92C_CR);
		reg |= R92C_CR_HCI_TXDMA_EN | R92C_CR_HCI_RXDMA_EN |
		    R92C_CR_TXDMA_EN | R92C_CR_RXDMA_EN | R92C_CR_PROTOCOL_EN |
		    R92C_CR_SCHEDULE_EN | R92C_CR_ENSEC | R92C_CR_CALTMR_EN;
		wr16(rd, wr, ctx, R92C_CR, (uint16_t)reg);
	}

	r->power_ok = 1;
	r->step = 3;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 4. DMA/LLT (OpenBSD: urtwn_llt_write, urtwn_llt_init, urtwn_dma_init) */
/* ------------------------------------------------------------------ */
#define R92C_LLT_INIT             0x1e0
#define R92C_LLT_INIT_DATA_M      0x000000ff
#define R92C_LLT_INIT_DATA_S      0
#define R92C_LLT_INIT_ADDR_M      0x0000ff00
#define R92C_LLT_INIT_ADDR_S      8
#define R92C_LLT_INIT_OP_M        0xc0000000
#define R92C_LLT_INIT_OP_S        30
#define R92C_LLT_INIT_OP_NO_ACTIVE 0
#define R92C_LLT_INIT_OP_WRITE    1

#define R92C_PBP                  0x104
#define R92C_TRXDMA_CTRL          0x10c
#define R92C_TRXFF_BNDY           0x114
#define R92C_RQPN                 0x200
#define R92C_TDECTRL              0x208
#define R92C_RQPN_NPQ             0x214
#define R92C_TXPKTBUF_BCNQ_BDNY   0x424
#define R92C_TXPKTBUF_MGQ_BDNY    0x425
#define R92C_TXPKTBUF_WMAC_LBK_BF_HD 0x45d

#define R92C_RQPN_HPQ_M           0x000000ff
#define R92C_RQPN_HPQ_S           0
#define R92C_RQPN_LPQ_M           0x0000ff00
#define R92C_RQPN_LPQ_S           8
#define R92C_RQPN_PUBQ_M          0x00ff0000
#define R92C_RQPN_PUBQ_S          16
#define R92C_RQPN_LD              0x80000000

#define R92C_PBP_PSRX_M           0x000f
#define R92C_PBP_PSRX_S           0
#define R92C_PBP_PSTX_M           0x00f0
#define R92C_PBP_PSTX_S           4
#define R92C_PBP_128              1

#define R92C_TRXDMA_CTRL_QMAP_M   0xfff0
#define R92C_TRXDMA_CTRL_QMAP_S   4
#define R92C_TRXDMA_CTRL_QMAP_3EP 0xf5b0
#define R92C_TRXDMA_CTRL_QMAP_HQ_NQ 0xfaf0
#define R92C_TRXDMA_CTRL_QMAP_HQ  0xfff0



int rtwn_seq_dma_init(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx, int ntx)
{
	uint32_t reg;
	uint16_t dmasize;
	int hqpages, lqpages, nqpages, pagecnt, boundary;
	int error, hashq, haslq, hasnq;

	hqpages = R88E_HQ_NPAGES;
	lqpages = R88E_LQ_NPAGES;
	nqpages = R88E_NQ_NPAGES;
	pagecnt = R88E_TX_PAGE_COUNT;
	dmasize = 0x2400;	/* R88E_MAX_RX_DMA_SIZE */
	boundary = pagecnt + 1;

	/* LLT: pakai versi vendor (InitLLTTable8188E @0xae125, argumen boundary = 0xaa). */
	if ((error = rtwn_llt_init_8188e(rd, wr, ctx, (uint32_t)boundary)) != 0) {
		return error;
	}

	hashq = hasnq = haslq = 0;
	switch (ntx) {
	case 3:
		haslq = 1;
		pagecnt -= lqpages;
		/* FALLTHROUGH */
	case 2:
		hasnq = 1;
		pagecnt -= nqpages;
		/* FALLTHROUGH */
	case 1:
		hashq = 1;
		pagecnt -= hqpages;
		break;
	default:
		break;
	}

	wr8(rd, wr, ctx, R92C_RQPN_NPQ, (uint8_t)(hasnq ? nqpages : 0));
	wr32(rd, wr, ctx, R92C_RQPN,
	    SM(R92C_RQPN_PUBQ, (uint32_t)pagecnt) |
	    SM(R92C_RQPN_HPQ, (uint32_t)(hashq ? hqpages : 0)) |
	    SM(R92C_RQPN_LPQ, (uint32_t)(haslq ? lqpages : 0)) |
	    R92C_RQPN_LD);

	wr8(rd, wr, ctx, R92C_TXPKTBUF_BCNQ_BDNY, (uint8_t)boundary);
	wr8(rd, wr, ctx, R92C_TXPKTBUF_MGQ_BDNY, (uint8_t)boundary);
	wr8(rd, wr, ctx, R92C_TXPKTBUF_WMAC_LBK_BF_HD, (uint8_t)boundary);
	wr8(rd, wr, ctx, R92C_TRXFF_BNDY, (uint8_t)boundary);
	wr8(rd, wr, ctx, R92C_TDECTRL + 1, (uint8_t)boundary);

	reg = rd16(rd, wr, ctx, R92C_TRXDMA_CTRL);
	reg &= ~R92C_TRXDMA_CTRL_QMAP_M;
	if (haslq) {
		reg |= R92C_TRXDMA_CTRL_QMAP_3EP;
	} else if (hashq) {
		reg |= hasnq ? R92C_TRXDMA_CTRL_QMAP_HQ_NQ : R92C_TRXDMA_CTRL_QMAP_HQ;
	}
	wr16(rd, wr, ctx, R92C_TRXDMA_CTRL, (uint16_t)reg);

	wr16(rd, wr, ctx, R92C_TRXFF_BNDY + 2, (uint16_t)(dmasize - 1));

	/* Ukuran halaman 128 byte (88E). */
	wr8(rd, wr, ctx, R92C_PBP,
	    (uint8_t)(SM(R92C_PBP_PSRX, R92C_PBP_128) |
	    SM(R92C_PBP_PSTX, R92C_PBP_128)));

	r->dma_ok = 1;
	r->step = 4;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 5. Upload firmware (OpenBSD: rtwn_load_firmware, urtwn_fw_loadpage) */
/* ------------------------------------------------------------------ */
int rtwn_seq_load_firmware(rtwn_seq_result_t *r, rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx, const uint8_t *fw, uint32_t fwlen)
{
	const uint8_t *ptr = fw;
	uint32_t len = fwlen;
	uint32_t reg;
	int mlen, ntries, page;

	/* Header firmware 32 byte (signature 0x88e…). */
	if (len >= 32) {
		uint16_t sig = (uint16_t)(fw[0] | (fw[1] << 8));
		uint16_t kind = (uint16_t)(sig >> 4);
		if (kind == 0x230 || kind == 0x88c || kind == 0x88e ||
		    kind == 0x88f || kind == 0x92c || kind == 0x92e) {
			ptr += 32;
			len -= 32;
		}
	}

	if (rd8(rd, wr, ctx, R92C_MCUFWDL) & R92C_MCUFWDL_RAM_DL_SEL) {
		wr8(rd, wr, ctx, R92C_MCUFWDL, 0);
	}

	/* Aktifkan mode download. */
	wr8(rd, wr, ctx, R92C_MCUFWDL,
	    (uint8_t)(rd8(rd, wr, ctx, R92C_MCUFWDL) | R92C_MCUFWDL_EN));
	wr32(rd, wr, ctx, R92C_MCUFWDL,
	    rd32(rd, wr, ctx, R92C_MCUFWDL) & ~R92C_MCUFWDL_ROM_DLEN);

	/* Reset checksum. */
	wr8(rd, wr, ctx, R92C_MCUFWDL,
	    (uint8_t)(rd8(rd, wr, ctx, R92C_MCUFWDL) | R92C_MCUFWDL_CHKSUM_RPT));

	for (page = 0; len > 0; page++) {
		uint32_t off = R92C_FW_START_ADDR;
		uint32_t left = (len > R92C_FW_PAGE_SIZE) ? R92C_FW_PAGE_SIZE : len;

		reg = rd32(rd, wr, ctx, R92C_MCUFWDL);
		reg = RW(reg, R92C_MCUFWDL_PAGE, (uint32_t)page);
		wr32(rd, wr, ctx, R92C_MCUFWDL, reg);

		while (left > 0) {
			if (left > 196) {
				mlen = 196;
			} else if (left > 4) {
				mlen = 4;
			} else {
				mlen = 1;
			}
			if (wr(ctx, (uint16_t)off, ptr, (uint16_t)mlen) != 0) {
				/*
				 * Tulis besar (196 byte) ditolak chip/stack.
				 *
				 * FALLBACK: pecah jadi blok kecil (4 byte).
				 * Register file RTL8188E bisa diakses per byte,
				 * jadi menulis 4 byte demi 4 byte pasti bisa.
				 *
				 * (22 Okt 2026: upload firmware versi pertama
				 *  gagal total — kemungkinan besar karena
				 *  vendor request dengan wLength=196 tidak
				 *  diterima untuk address 0x1000+.)
				 */
				if (mlen > 4) {
					uint32_t rem = left;
					uint32_t o = off;
					const uint8_t *pt = ptr;
					while (rem > 0) {
						int chunk = (rem > 4) ? 4 : (int)rem;
						if (wr(ctx, (uint16_t)o, pt,
						    (uint16_t)chunk) != 0) {
							r->fwlen = (uint32_t)(fwlen - len);
							return -1;
						}
						o += chunk;
						pt += chunk;
						rem -= chunk;
					}
					off = o;
					ptr = pt;
					left = 0;
					break;
				}
				/*
				 * Diagnostik: simpan berapa byte yang SUDAH
				 * berhasil ditulis, supaya driver bisa
				 * melaporkan di mana upload ini macet.
				 */
				r->fwlen = (uint32_t)(fwlen - len);
				return -1;
			}
			off += mlen;
			ptr += mlen;
			left -= mlen;
		}
		len -= (len > R92C_FW_PAGE_SIZE) ? R92C_FW_PAGE_SIZE : len;
	}

	/* Tunggu laporan checksum. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (rd32(rd, wr, ctx, R92C_MCUFWDL) & R92C_MCUFWDL_CHKSUM_RPT) {
			break;
		}
		RTWN_DELAY_MS(1);
	}
	if (ntries == 1000) {
		return -1;
	}

	/* Nonaktifkan mode download. */
	wr8(rd, wr, ctx, R92C_MCUFWDL,
	    (uint8_t)(rd8(rd, wr, ctx, R92C_MCUFWDL) & ~R92C_MCUFWDL_EN));
	wr8(rd, wr, ctx, R92C_MCUFWDL + 1, 0);

	reg = rd32(rd, wr, ctx, R92C_MCUFWDL);
	reg = (reg & ~R92C_MCUFWDL_WINTINI_RDY) | R92C_MCUFWDL_RDY;
	wr32(rd, wr, ctx, R92C_MCUFWDL, reg);

	/* Reset MCU (OpenBSD: rtwn_r88e_fw_reset). */
	wr8(rd, wr, ctx, R92C_RSV_CTRL,
	    (uint8_t)(rd8(rd, wr, ctx, R92C_RSV_CTRL) & ~R92C_RSV_CTRL_WLOCK_00));
	wr16(rd, wr, ctx, R92C_RSV_CTRL,
	    (uint16_t)(rd16(rd, wr, ctx, R92C_RSV_CTRL) & ~R88E_RSV_CTRL_MCU_RST));
	wr16(rd, wr, ctx, R92C_SYS_FUNC_EN,
	    (uint16_t)(rd16(rd, wr, ctx, R92C_SYS_FUNC_EN) &
	    ~R92C_SYS_FUNC_EN_CPUEN));
	wr8(rd, wr, ctx, R92C_RSV_CTRL,
	    (uint8_t)(rd8(rd, wr, ctx, R92C_RSV_CTRL) & ~R92C_RSV_CTRL_WLOCK_00));
	wr16(rd, wr, ctx, R92C_RSV_CTRL,
	    (uint16_t)(rd16(rd, wr, ctx, R92C_RSV_CTRL) | R88E_RSV_CTRL_MCU_RST));
	wr16(rd, wr, ctx, R92C_SYS_FUNC_EN,
	    (uint16_t)(rd16(rd, wr, ctx, R92C_SYS_FUNC_EN) | R92C_SYS_FUNC_EN_CPUEN));

	/* Tunggu firmware siap. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (rd32(rd, wr, ctx, R92C_MCUFWDL) & R92C_MCUFWDL_WINTINI_RDY) {
			break;
		}
		RTWN_DELAY_MS(1);
	}
	if (ntries == 1000) {
		return -1;
	}

	r->fw_ok = 1;
	r->fwlen = fwlen;
	r->step = 5;
	return 0;
}

