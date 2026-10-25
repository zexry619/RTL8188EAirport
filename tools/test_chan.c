/*
 * test_chan.c — uji offline pengaturan kanal 8188E.
 *
 * Memverifikasi urutan & nilai register yang ditulis, memakai mock
 * register (read/write callback).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtwn_chan.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

#define MOCK_REGS 4096
static uint8_t regs[MOCK_REGS];
static uint8_t regs4[MOCK_REGS];	/* bayangan register 32-bit */
static int wr_log[256];
static int wr_log_len;

static int mock_rd(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	(void)ctx;
	uint8_t *b = (uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		uint16_t a = (uint16_t)(addr + i);
		b[i] = (a < MOCK_REGS) ? regs4[a] : 0xff;
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
			regs4[a] = b[i];
		}
		if (wr_log_len < 256) {
			wr_log[wr_log_len++] = (int)a;
		}
	}
	return 0;
}

static uint32_t reg32(uint16_t a)
{
	return (uint32_t)regs4[a] | ((uint32_t)regs4[a + 1] << 8) |
	    ((uint32_t)regs4[a + 2] << 16) | ((uint32_t)regs4[a + 3] << 24);
}

int main(void)
{
	printf("══ Test set kanal 8188E ══\n");

	memset(regs, 0, sizeof(regs));
	memset(regs4, 0, sizeof(regs4));
	wr_log_len = 0;

	/* reg awal: BWOPMODE tanpa 20MHz, FPGA RFMOD dengan flag 40MHz */
	regs4[0x603] = 0x00;
	regs4[0x800] = 0x01;	/* RFMOD_40MHZ aktif */
	regs4[0x801] = 0x00;
	regs4[0x900] = 0x01;
	regs4[0x901] = 0x00;

	CHECK(rtwn_set_chan_8188e(mock_rd, mock_wr, NULL, 6) == 0,
	    "set kanal 6 berhasil");

	printf("\n── Nilai register sesudahnya ──\n");
	printf("     BWOPMODE (0x603)    = 0x%02x\n", regs4[0x603]);
	printf("     FPGA0_RFMOD(0x800)  = 0x%08x\n", reg32(0x800));
	printf("     FPGA1_RFMOD(0x900)  = 0x%08x\n", reg32(0x900));
	printf("     LSSI_PARAM(0x840)   = 0x%08x\n", reg32(0x840));
	printf("     HSSI_PARAM2(0x824)  = 0x%08x\n", reg32(0x824));
	printf("     HSSI_PARAM1(0x820)  = 0x%08x\n", reg32(0x820));
	printf("     LSSI_READBK(0x8a0)  = 0x%08x\n\n", reg32(0x8a0));

	CHECK((regs4[0x603] & 0x04) == 0x04, "BWOPMODE: bit 20MHz diset");
	CHECK((reg32(0x800) & 0x01) == 0, "FPGA0 RFMOD: flag 40MHz dibersihkan");
	CHECK((reg32(0x900) & 0x01) == 0, "FPGA1 RFMOD: flag 40MHz dibersihkan");

	/* LSSI_PARAM(0x840): addr<<20 | data<<0
	 * addr = 0x18 (R92C_RF_CHNLBW), data = (old & ~0xfff) | chan | 0xc00
	 * old = 0 (mock readback) → data = 6 | 0xc00 = 0xc06 */
	{
		uint32_t lssi = reg32(0x840);
		uint32_t addr = (lssi >> 20) & 0xff;
		uint32_t data = lssi & 0xfffff;
		printf("     → RF addr=0x%02x data=0x%05x\n\n", addr, data);
		CHECK(addr == 0x18, "RF alamat = 0x18 (R92C_RF_CHNLBW)");
		/* channel = bit 0..9 (R92C_RF_CHNLBW_CHNL_M = 0x3ff)
		 * BW20     = bit 10..11 (R88E_RF_CHNLBW_BW20 = 0xc00) */
		CHECK((data & 0x3ff) == 6, "data: nomor kanal 6 di bit 0..9");
		CHECK((data & 0xc00) == 0xc00, "data: bit BW20 (0xc00) diset");
	}

	/* kasus batas: kanal di luar 1..14 */
	CHECK(rtwn_set_chan_8188e(mock_rd, mock_wr, NULL, 0) != 0,
	    "kanal 0 DITOLAK");
	CHECK(rtwn_set_chan_8188e(mock_rd, mock_wr, NULL, 15) != 0,
	    "kanal 15 DITOLAK");

	printf("\n%s\n", fails == 0 ? "✅ semua uji lulus" : "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
