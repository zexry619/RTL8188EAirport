/*
 * test_8051.c — uji offline sequence pause/enable 8051 (regresi root cause EFUSE).
 *
 * ROOT CAUSE (22 Okt 2026): implementasi lama memakai register 0x093
 * (cabang cardType == 0x32), padahal 8188E (cardType 0x0e) memakai
 * 0x1c / 0x1d / 0x03. Akibatnya 8051 tak pernah di-reset dengan benar →
 * trigger offload tidak mengisi buffer → EFUSE kosong (0xff).
 *
 * Test ini memverifikasi register yang benar-benar ditulis untuk tiap gaya.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtwn_seq_efuse.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

#define MOCK_REGS 4096
static uint8_t regs[MOCK_REGS];
static int wrote_093;
static int wrote_01c;
static int wrote_01d;
static int wrote_003;

static int mock_rd(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	(void)ctx;
	uint8_t *b = (uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		b[i] = (a < MOCK_REGS) ? regs[a] : 0xff;
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
			regs[a] = b[i];
		}
		switch (a) {
		case 0x093: wrote_093++; break;
		case 0x01c: wrote_01c++; break;
		case 0x01d: wrote_01d++; break;
		case 0x003: wrote_003++; break;
		default: break;
		}
	}
	return 0;
}

static void reset_counters(void)
{
	wrote_093 = wrote_01c = wrote_01d = wrote_003 = 0;
}

int main(void)
{
	printf("══ Test sequence 8051 (regresi root cause EFUSE) ══\n");

	/* ── Gaya A: 0x1d &= ~0x01 (pause) / |= 0x01 (enable) ── */
	printf("\n── Gaya A (cardType 0x0e) ──\n");
	memset(regs, 0, sizeof(regs));
	regs[0x01c] = 0xff;	/* bit1 = 1 → harus dibersihkan */
	regs[0x01d] = 0xff;	/* bit0 = 1 → pause harus clearest */
	regs[0x003] = 0xff;	/* bit2 = 1 → pause harus clearest */
	reset_counters();

	rtwn_8051_pause_test(mock_rd, mock_wr, NULL, RTWN_8051_STYLE_A);
	CHECK(wrote_01c > 0, "pause menulis register 0x01c");
	CHECK(wrote_01d > 0, "pause menulis register 0x01d");
	CHECK(wrote_003 > 0, "pause menulis register 0x003");
	CHECK(wrote_093 == 0, "pause TIDAK menulis 0x093 (itu gaya chip 0x32)");
	CHECK((regs[0x01c] & 0x02) == 0, "0x01c: bit1 dibersihkan");
	CHECK((regs[0x01d] & 0x01) == 0, "0x01d: bit0 dibersihkan (pause)");
	CHECK((regs[0x003] & 0x04) == 0, "0x003: bit2 dibersihkan (pause)");

	reset_counters();
	rtwn_8051_enable_test(mock_rd, mock_wr, NULL, RTWN_8051_STYLE_A);
	CHECK((regs[0x01d] & 0x01) == 0x01, "0x01d: bit0 diset (enable)");
	CHECK((regs[0x003] & 0x04) == 0x04, "0x003: bit2 diset (enable)");
	CHECK(wrote_093 == 0, "enable TIDAK menulis 0x093");

	/* ── Gaya B: 0x1d &= ~0x08 / |= 0x08 ── */
	printf("\n── Gaya B (cardType 0x0f / 0x10) ──\n");
	memset(regs, 0, sizeof(regs));
	regs[0x01c] = 0xff;
	regs[0x01d] = 0xff;
	regs[0x003] = 0xff;
	reset_counters();

	rtwn_8051_pause_test(mock_rd, mock_wr, NULL, RTWN_8051_STYLE_B);
	CHECK((regs[0x01d] & 0x08) == 0, "0x01d: bit3 dibersihkan (pause, gaya B)");
	CHECK((regs[0x01d] & 0x01) == 0x01,
	    "0x01d: bit0 TIDAK disentuh di gaya B");

	reset_counters();
	rtwn_8051_enable_test(mock_rd, mock_wr, NULL, RTWN_8051_STYLE_B);
	CHECK((regs[0x01d] & 0x08) == 0x08, "0x01d: bit3 diset (enable, gaya B)");
	CHECK(wrote_093 == 0, "gaya B TIDAK menulis 0x093");

	/* ── Gaya lama (chip 0x32) — harus tetap tersedia ── */
	printf("\n── Gaya lama (cardType 0x32) ──\n");
	memset(regs, 0, sizeof(regs));
	regs[0x093] = 0xff;
	reset_counters();
	rtwn_8051_pause_test(mock_rd, mock_wr, NULL, RTWN_8051_STYLE_32);
	CHECK(wrote_093 > 0, "gaya 0x32 menulis 0x093");
	CHECK((regs[0x093] & 0x01) == 0, "0x093: bit0 dibersihkan");
	CHECK(wrote_003 == 0, "gaya 0x32 TIDAK menyentuh 0x003");

	printf("\n%s\n", fails == 0 ? "✅ semua uji lulus" : "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
