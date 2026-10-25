/*
 * test_seq_safety.c — regression test untuk bug keamanan di lapisan
 * rtwn_seq / rtwn_seq_efuse (audit 22 Okt 2026).
 *
 * Bug yang diuji:
 *
 *  F1  rtwn_seq_efuse.c — header offload dengan nilai < 8 menyebabkan
 *      underflow unsigned → n = 536.870.911 → loop tak berujung dan
 *      `off` bisa jadi NEGATIF → memcpy menulis DI LUAR array raw
 *      (stack smash di kernel).
 *
 *  F4  rtwn_seq.c — batas loop `(uint16_t)len` dengan len = 256
 *      menjadi 0 → loop TIDAK PERNAH jalan → EFUSE dilaporkan "OK"
 *      dengan MAC FF:FF:FF:FF:FF:FF.
 *
 *  F5  rtwn_seq.c — timeout chip (0xff) tidak bisa dibedakan dari
 *      nilai 0xff yang sah → chip mati dilaporkan sukses.
 *
 * Semua diuji dengan mock read/write (tanpa hardware).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rtwn_seq.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

/* Register offload (sama dengan rtwn_seq_efuse.c). */
#define OFF_CTRL	0x0f0
#define OFF_TRIG	0x088
#define OFF_ADDR	0x140
#define OFF_STAT	0x143
#define OFF_HDR		0x144
#define OFF_DATA	0x148

/* Register 92C (dari r92creg.h). */
#define EFUSE_CTRL_92C	0x030

/* ------------------------------------------------------------------ */
/* Mock register                                                       */
/* ------------------------------------------------------------------ */

#define MOCK_REGS 4096

typedef struct {
	uint8_t regs[MOCK_REGS];
	int	reads;
	int	writes;
} mock_t;

static int mock_rd(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	mock_t *m = (mock_t *)ctx;
	uint8_t *b = (uint8_t *)buf;

	m->reads++;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		b[i] = (a < MOCK_REGS) ? m->regs[a] : 0xff;
	}
	return 0;
}

static int mock_wr(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
	mock_t *m = (mock_t *)ctx;
	const uint8_t *b = (const uint8_t *)buf;

	m->writes++;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		if (a < MOCK_REGS) {
			m->regs[a] = b[i];
		}
	}
	return 0;
}

/*
 * Reader yang menyimulasikan chip EFUSE 92C yang sehat:
 *   - R92C_EFUSE_CTRL (0x030) dibaca 4 byte → bit VALID (0x80000000)
 *     diset dan byte data = 0x42.
 *   - register lain → 0x42.
 *
 * Dipakai untuk membuktikan loop EFUSE benar-benar jalan (F4).
 */
static int rd_always_valid(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	mock_t *m = (mock_t *)ctx;
	uint8_t *b = (uint8_t *)buf;

	m->reads++;
	for (uint16_t i = 0; i < len; i++) {
		b[i] = 0x42;
	}
	if (addr == EFUSE_CTRL_92C && len >= 4) {
		/* byte 3 = bit VALID */
		b[3] = 0x80;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* F1 — header offload nakal harus ditolak, TIDAK boleh menulis OOB      */
/* ------------------------------------------------------------------ */

static void test_offload_bad_header(void)
{
	printf("\n── Offload: header nakal (F1) ──\n");

	for (int bad = 0; bad < 8; bad++) {
		mock_t m;
		memset(&m, 0, sizeof(m));

		/*
		 * Simulasikan "chip belum mengisi header":
		 *   - OFF_TRIG bit1 = 0 (busy sudah selesai)
		 *   - OFF_STAT bit0 = 1 (data tersedia)
		 *   - OFF_HDR low16 = bad  (nilai < 8 → underflow dulu)
		 */
		m.regs[OFF_TRIG] = 0x00;
		m.regs[OFF_STAT] = 0x01;
		m.regs[OFF_HDR] = (uint8_t)bad;
		m.regs[OFF_HDR + 1] = 0x00;
		m.regs[OFF_DATA] = 0x11;
		m.regs[OFF_DATA + 1] = 0x22;
		m.regs[OFF_DATA + 2] = 0x33;
		m.regs[OFF_DATA + 3] = 0x44;

		rtwn_seq_result_t r;
		memset(&r, 0, sizeof(r));

		/*
		 * Kalau F1 belum diperbaiki: (a) looping 536 juta iterasi,
		 * atau (b) `off` negatif → memcpy menulis 8 byte SEBELUM
		 * array raw[] → stack smash. Di user space ini akan menabrak
		 * stack guard / menulis ke variabel lain.
		 */
		int rc = rtwn_seq_read_efuse_offload(&r, mock_rd, mock_wr, &m);

		CHECK(rc != 0, "header < 8 DITOLAK (tidak diproses)");
		CHECK(r.efuse_ok == 0, "tidak dilaporkan EFUSE OK");
		CHECK(m.reads < 100000, "tidak ada loop tak berujung");
	}

	/* Header valid (>= 8) harus diproses tanpa crash. */
	{
		mock_t m;
		memset(&m, 0, sizeof(m));
		m.regs[OFF_TRIG] = 0x00;
		m.regs[OFF_STAT] = 0x01;
		m.regs[OFF_HDR] = 0x08;	/* hdr = 8 → n = 0 blok */
		m.regs[OFF_HDR + 1] = 0x00;

		rtwn_seq_result_t r;
		memset(&r, 0, sizeof(r));
		(void)rtwn_seq_read_efuse_offload(&r, mock_rd, mock_wr, &m);
		CHECK(1, "header valid diproses tanpa crash");
	}
}

/* ------------------------------------------------------------------ */
/* F4/F5 — loop EFUSE harus jalan & timeout harus error                 */
/* ------------------------------------------------------------------ */

static void test_efuse_dead_chip(void)
{
	printf("\n── EFUSE: chip mati (F4/F5) ──\n");

	mock_t m;
	memset(&m, 0, sizeof(m));	/* semua register = 0 → tidak merespons */

	rtwn_seq_result_t r;
	memset(&r, 0, sizeof(r));

	int rc = rtwn_seq_read_efuse(&r, mock_rd, mock_wr, &m);

	/*
	 * Chip mati → harus GAGAL.
	 * Sebelum F4/F5: loop tidak jalan sama sekali → raw[] tetap 0xff
	 * → MAC FF:FF:FF:FF:FF:FF dilaporkan "EFUSE OK".
	 */
	CHECK(rc != 0, "chip tidak merespons → error (bukan sukses)");
	CHECK(r.efuse_ok == 0, "efuse_ok = 0 untuk chip mati");

	int bogus = 0;
	for (int i = 0; i < 6; i++) {
		if (r.mac[i] == 0xff || r.mac[i] == 0x00) {
			bogus++;
		}
	}
	CHECK(bogus == 6, "MAC bukan data palsu (semua 0xff/0x00)");
}

static void test_efuse_loop_runs(void)
{
	printf("\n── EFUSE: loop benar-benar jalan (F4) ──\n");

	mock_t m;
	memset(&m, 0, sizeof(m));

	rtwn_seq_result_t r;
	memset(&r, 0, sizeof(r));

	(void)rtwn_seq_read_efuse(&r, rd_always_valid, mock_wr, &m);

	/*
	 * Sebelum F4: `(uint16_t)256 == 0` → loop tidak jalan → 0 read.
	 * Sekarang harus ada hundreds of reads.
	 */
	CHECK(m.reads > 100, "loop EFUSE benar-benar jalan");

	/*
	 * Yang diuji di sini:
	 *  1. Loop EFUSE benar-benar jalan. Sebelum F4 batasnya
	 *     `(uint16_t)len` dengan len = 256 → jadi 0 → loop tidak
	 *     pernah jalan (0 read) → MAC dilaporkan FF:FF:FF:FF:FF:FF.
	 *  2. Sanity check MAC bekerja: gambar sintetis all-0x42 tidak
	 *     menghasilkan MAC valid di offset 215 → ditolak (efuse_ok=0),
	 *     bukan dilaporkan "OK" dengan MAC palsu.
	 */
	printf("     MAC = %02x:%02x:%02x:%02x:%02x:%02x  (efuse_ok=%d)\n",
	    r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5],
	    r.efuse_ok);
	CHECK(m.reads > 100, "loop EFUSE benar-benar jalan");
	CHECK(r.efuse_ok == 0, "MAC sintetis tidak valid → ditolak (efuse_ok=0)");
}

int main(void)
{
	printf("══ Test keamanan rtwn_seq (regression F1/F4/F5) ══\n");

	test_offload_bad_header();
	test_efuse_dead_chip();
	test_efuse_loop_runs();

	printf("\n%s\n", fails == 0 ? "✅ semua uji keamanan lulus" :
	    "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
