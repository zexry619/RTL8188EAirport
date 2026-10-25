/*
 * test_hal_tables.c — uji offline untuk tabel & applier HAL 8188E.
 *
 * Menguji:
 *  1. Isi tabel hasil ekstraksi dari binary vendor (sanity check).
 *  2. Semantik loop applier: tulis biasa, entri kontrol (resume/toggle),
 *     entri bersyarat (cabang MAC).
 *
 * Tidak ada hardware yang disentuh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtwn_hal_8188e.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

/* ---- mock register ---- */
#define MOCK_REGS 4096
static uint8_t mock_regs[MOCK_REGS];
static int mock_writes;

static int mock_rd(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	(void)ctx;
	uint8_t *b = (uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		b[i] = (a < MOCK_REGS) ? mock_regs[a] : 0xff;
	}
	return 0;
}

static int mock_wr(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
	(void)ctx;
	const uint8_t *b = (const uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		if (a < MOCK_REGS) {
			mock_regs[a] = b[i];
		}
	}
	mock_writes++;
	return 0;
}

/* ------------------------------------------------------------------ */

static void test_table_content(void)
{
	printf("\n── Isi tabel (hasil ekstraksi binary vendor) ──\n");

	CHECK(sizeof(rtwn_mp_8188e_mac_reg) / sizeof(rtwn_mp_8188e_mac_reg[0]) == 106,
	    "mac_reg: 106 entri (sesuai loop vendor 0..0xD3)");
	CHECK(sizeof(rtwn_mp_8188e_phy_reg) / sizeof(rtwn_mp_8188e_phy_reg[0]) == 704,
	    "phy_reg: 704 entri");
	CHECK(sizeof(rtwn_mp_8188e_agc_tab) / sizeof(rtwn_mp_8188e_agc_tab[0]) == 976,
	    "agc_tab: 976 entri");
	CHECK(sizeof(rtwn_mp_8188e_radioa) / sizeof(rtwn_mp_8188e_radioa[0]) == 660,
	    "radioa: 660 entri");

	/* Dua entri pertama mac_reg dari byte mentah vendor:
	 *   [0] odd=0x26 val=0x41   [1] odd=0x27 val=0x35 */
	CHECK(rtwn_mp_8188e_mac_reg[0].odd == 0x26 &&
	    rtwn_mp_8188e_mac_reg[0].val == 0x41,
	    "mac_reg[0] = reg 0x26 val 0x41");
	CHECK(rtwn_mp_8188e_mac_reg[1].odd == 0x27 &&
	    rtwn_mp_8188e_mac_reg[1].val == 0x35,
	    "mac_reg[1] = reg 0x27 val 0x35");

	/*
	 * GUARD PENTING: SEMUA entri "tulis biasa" di SEMUA tabel harus
	 * punya reg < 0x1000 (register RTL8188E berada di bawah 0x1000).
	 *
	 * Ini menangkap tabel yang formatnya berbeda dan TIDAK boleh
	 * diapplier — misalnya _array_mp_8188e_txpwr_lmt yang isinya
	 * uint16 (0x8edd, 0x0024, ...) → kalau ikut diapplier, driver
	 * akan menulis ke register 0x248edd = sampah.
	 */
	{
		struct { const char *name; const rtwn_mp_entry_t *tab; size_t n; } all[] = {
			{ "mac_reg",     rtwn_mp_8188e_mac_reg,
			  sizeof(rtwn_mp_8188e_mac_reg)/sizeof(rtwn_mp_8188e_mac_reg[0]) },
			{ "phy_reg",     rtwn_mp_8188e_phy_reg,
			  sizeof(rtwn_mp_8188e_phy_reg)/sizeof(rtwn_mp_8188e_phy_reg[0]) },
			{ "agc_tab",     rtwn_mp_8188e_agc_tab,
			  sizeof(rtwn_mp_8188e_agc_tab)/sizeof(rtwn_mp_8188e_agc_tab[0]) },
			{ "radioa",      rtwn_mp_8188e_radioa,
			  sizeof(rtwn_mp_8188e_radioa)/sizeof(rtwn_mp_8188e_radioa[0]) },
		};
		int bad = 0;
		for (size_t t = 0; t < sizeof(all)/sizeof(all[0]); t++) {
			for (size_t i = 0; i < all[t].n; i++) {
				uint32_t o = all[t].tab[i].odd;
				if (o < RTWN_MP_PLAIN_MAX && o >= 0x1000) {
					if (bad < 3) {
						printf("     ⚠️  %s[%zu] reg=0x%x\n",
						    all[t].name, i, o);
					}
					bad++;
				}
			}
		}
		CHECK(bad == 0, "semua tabel: entri tulis-biasa punya reg < 0x1000");
	}
}

/* ---- semantik applier dengan tabel buatan ---- */

static void test_applier_plain(void)
{
	printf("\n── Applier: tulis biasa ──\n");

	const rtwn_mp_entry_t tab[] = {
		{ 0x0100u, 0xaa },
		{ 0x0101u, 0xbb },
	};
	rtwn_hal_ctx_t hc;
	memset(mock_regs, 0, sizeof(mock_regs));
	mock_writes = 0;

	rtwn_hal_init(&hc, mock_rd, mock_wr, NULL);
	int n = rtwn_hal_apply_table(&hc, tab, 2);

	CHECK(n == 2, "2 register ditulis");
	CHECK(mock_regs[0x100] == 0xaa && mock_regs[0x101] == 0xbb,
	    "nilai register benar");
}

static void test_applier_control(void)
{
	printf("\n── Applier: entri kontrol (resume / toggle) ──\n");

	/*
	 * Urutan vendor:
	 *   write A ; ctrl(toggle, off) ; write B   → B TIDAK ditulis
	 *   ctrl(resume) ; write C                 → C ditulis
	 */
	const rtwn_mp_entry_t tab[] = {
		{ 0x0200u, 0x11 },
		{ 0x80000002u, 0x00 },	/* toggle → apply = !cond = !false = true?? */
		{ 0x0201u, 0x22 },
		{ 0x90000001u, 0x00 },	/* resume → apply = true, cond = false */
		{ 0x0202u, 0x33 },
	};
	rtwn_hal_ctx_t hc;
	memset(mock_regs, 0, sizeof(mock_regs));
	mock_writes = 0;

	rtwn_hal_init(&hc, mock_rd, mock_wr, NULL);
	int n = rtwn_hal_apply_table(&hc, tab, 5);

	printf("     ditulis: %d, reg 0x200=%02x 0x201=%02x 0x202=%02x\n",
	    n, mock_regs[0x200], mock_regs[0x201], mock_regs[0x202]);

	/* intisial: apply=true, cond=false
	 * entri 0x80000002 → top2 = 2 → apply = !cond = true → tulis 0x201
	 * entri 0x90000001 → top2 = 3 → apply = true → tulis 0x202
	 * Jadi semua 3 ditulis. */
	CHECK(n == 3, "3 register ditulis (toggle dengan cond=false → apply)");
	CHECK(mock_regs[0x200] == 0x11 && mock_regs[0x201] == 0x22 &&
	    mock_regs[0x202] == 0x33, "semua nilai masuk register");
}

static void test_applier_conditional(void)
{
	printf("\n── Applier: entri bersyarat (cabang MAC) ──\n");

	/*
	 * Entri bersyarat 0x40000000 membandingkan window MAC dengan pola
	 * yang disimpan oleh entri kontrol 0x8xxxxxxx / 0x9xxxxxxx.
	 *
	 * Semantik (dari loop vendor):
	 *   - 0x9xxxxxxx → MENYIMPAN pola: saved_val = val, saved_odd = odd
	 *   - 0x8xxxxxxx → juga menyimpan pola
	 *   - 0xAxxxxxxx / 0xExxxxxxxx → toggle apply = !cond (tidak menyimpan)
	 *   - 0xBxxxxxxx / 0xFxxxxxxx → resume: apply = true, cond = false
	 *   - 0x4xxxxxxx → bandingkan: mask = low byte saved_odd
	 *     (bit1/2/3 menentukan byte mana yang wildcard 0xff),
	 *     lalu cocokkan dengan byte window MAC.
	 *
	 * Di sini: pola 0x01 dengan mask 0x01 → hanya bit0 byte0 yang
	 * dibandingkan. MAC byte0 = 0x98 → bit0 = 0, pola bit0 = 1
	 * → TIDAK COCOK → apply = false → write berikutnya dilewati.
	 */
	const rtwn_mp_entry_t tab[] = {
		{ 0x90000001u, 0x01 },	/* simpan pola: val=0x01, mask=0x01 */
		{ 0x0300u, 0x55 },	/* ditulis (apply masih true) */
		{ 0x40000000u, 0x00 },	/* bersyarat → tidak cocok → apply=false */
		{ 0x0301u, 0x66 },	/* harus DILEWATI */
		{ 0xB0000001u, 0x00 },	/* resume (0xBxxxxxxx) → apply = true */
		{ 0x0302u, 0x77 },	/* ditulis */
	};
	rtwn_hal_ctx_t hc;
	static const uint8_t mac[6] = { 0x98, 0x03, 0x8e, 0x9e, 0xbc, 0x24 };

	memset(mock_regs, 0, sizeof(mock_regs));
	mock_writes = 0;

	rtwn_hal_init(&hc, mock_rd, mock_wr, NULL);
	rtwn_hal_set_mac(&hc, mac);
	int n = rtwn_hal_apply_table(&hc, tab, 6);

	printf("     ditulis: %d, reg 0x300=%02x 0x301=%02x 0x302=%02x\n",
	    n, mock_regs[0x300], mock_regs[0x301], mock_regs[0x302]);

	CHECK(mock_regs[0x300] == 0x55, "register sebelum cabang bersyarat ditulis");
	CHECK(mock_regs[0x301] == 0x00, "register setelah cabang gagal DILEWATI");
	CHECK(mock_regs[0x302] == 0x77, "register setelah resume ditulis");
}

static void test_real_table_applies(void)
{
	printf("\n── Applier: tabel mac_reg asli ──\n");

	rtwn_hal_ctx_t hc;
	static const uint8_t mac[6] = { 0x98, 0x03, 0x8e, 0x9e, 0xbc, 0x24 };

	memset(mock_regs, 0, sizeof(mock_regs));
	mock_writes = 0;

	rtwn_hal_init(&hc, mock_rd, mock_wr, NULL);
	rtwn_hal_set_mac(&hc, mac);

	int n = rtwn_hal_apply_table(&hc, rtwn_mp_8188e_mac_reg,
	    sizeof(rtwn_mp_8188e_mac_reg) / sizeof(rtwn_mp_8188e_mac_reg[0]));

	printf("     register ditulis: %d dari 106 entri\n", n);
	CHECK(n > 60, "sebagian besar entri mac_reg benar-benar ditulis");
	CHECK(mock_regs[0x26] == 0x41, "reg 0x26 = 0x41 (entri pertama)");
	CHECK(mock_regs[0x27] == 0x35, "reg 0x27 = 0x35 (entri kedua)");
}

int main(void)
{
	printf("══ Test tabel & applier HAL 8188E ══\n");

	test_table_content();
	test_applier_plain();
	test_applier_control();
	test_applier_conditional();
	test_real_table_applies();

	printf("\n%s\n", fails == 0 ? "✅ semua uji lulus" : "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
