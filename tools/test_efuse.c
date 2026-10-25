/*
 * test_efuse.c — validasi algoritma parser EFUSE TANPA device.
 *
 * Mock mengemulasikan register EFUSE_CTRL (program address → baca byte),
 * persis seperti hardware. Image EFUSE yang digenerated merupakan bentuk
 * realistis map 8188E: 64 blok,多数 blok SPARSE (mask = 0x0f → 0 byte data),
 * hanya beberapa blok yang "padat" (mask = 0 → 8 byte data).
 *
 * Blok >= 16 memakai EXTENDED HEADER (2 byte):
 *   byte1 = 0x0f | ((off & 0x07) << 5)      → (cur & 0x1f) == 0x0f
 *   byte2 = ((off & 0x38) << 1) | mask        → (nxt & 0x0f) == mask
 *   off = ((nxt >> 1) & 0x78) | (cur >> 5)
 *
 * Build:
 *   clang -O2 -o /tmp/test_efuse tools/test_efuse.c src/urtwn/rtwn_seq.c -Isrc/urtwn
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rtwn_seq.h"

#define EFUSE_CTRL 0x030
#define EFUSE_CTRL_VALID 0x80000000

static uint8_t g_raw[512];
static int g_raw_len;
static uint16_t g_addr;

static int mock_read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	uint8_t *b = (uint8_t *)buf;
	uint32_t v = 0;

	(void)ctx;
	if (addr == EFUSE_CTRL) {
		v = (g_addr < g_raw_len) ? g_raw[g_addr] : 0xff;
		v |= EFUSE_CTRL_VALID;
	} else {
		v = 0xffffffff;
	}
	for (uint16_t i = 0; i < len; i++) {
		b[i] = (uint8_t)((v >> (8 * i)) & 0xff);
	}
	return 0;
}

static int mock_write(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
	const uint8_t *b = (const uint8_t *)buf;
	uint32_t v = 0;

	(void)ctx;
	for (uint16_t i = 0; i < len && i < 4; i++) {
		v |= (uint32_t)b[i] << (8 * i);
	}
	if (addr == EFUSE_CTRL) {
		g_addr = (uint16_t)((v >> 8) & 0x3ff);
	}
	return 0;
}

/*
 * Tabel mask ASLI 8188E MUSB, diambil dari binary vendor:
 *   RtWlanU::_Array_MP_8188E_MUSB  @0x376f70
 *   (32 byte; tiap byte = mask blok ke-n, bit 1 = slot TIDAK ada)
 *
 * ff f3 00 00 00 00 00 00 00 00 0f f3 ff ff 30 00 00 00 f7 00 00 00 00 ...
 */
static const uint8_t g_real_mask[32] = {
	0xff, 0xf3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x0f, 0xf3, 0xff, 0xff, 0x30,
	0x00, 0x00, 0x00, 0xf7, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Mask efektif (4 bit) untuk blok ke-n. */
static uint8_t block_mask(int block)
{
	if (block < 32) {
		return (uint8_t)(g_real_mask[block] & 0x0f);
	}
	return 0x00;
}

/* Nilai data synthetic: v = 0xA000 | block → byte pertama = v&0xff, kedua = v>>8 */
static uint16_t marker(int block)
{
	return (uint16_t)(0xA000 | block);
}

static void build_image(void)
{
	int pos = 0;
	int block;

	memset(g_raw, 0xff, sizeof(g_raw));
	for (block = 0; block < 32; block++) {
		uint8_t off = (uint8_t)block;
		uint8_t mask = block_mask(block);
		int i;

		if (off < 16) {
			g_raw[pos++] = (uint8_t)((off << 4) | mask);
		} else {
			g_raw[pos++] = (uint8_t)(0x0f | ((off & 0x07) << 5));
			g_raw[pos++] = (uint8_t)(((off & 0x38) << 1) | mask);
		}
		for (i = 0; i < 4; i++) {
			if (mask & (1 << i)) {
				continue;	/* slot absent */
			}
			uint16_t v = marker(block);
			g_raw[pos++] = (uint8_t)(v & 0xff);
			g_raw[pos++] = (uint8_t)(v >> 8);
		}
	}
	g_raw_len = pos;
}

int main(void)
{
	rtwn_seq_result_t r;
	int fails = 0;

	build_image();
	printf("=== uji parser EFUSE (image sintetis, %d byte) ===\n", g_raw_len);

	memset(&r, 0, sizeof(r));
	rtwn_seq_read_efuse(&r, mock_read, mock_write, NULL);

	printf("  MAC      = %02x:%02x:%02x:%02x:%02x:%02x\n",
	    r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
	printf("  xtal=0x%02x  regulatory=%u  channel_plan=%u\n",
	    r.xtal, r.regulatory, r.channel_plan);
	printf("  ekspektasi: MAC=a0:1b:1b:1b:1b:1b  xtal=0xa0  chplan=0x17\n");

	/* rom[185] = blok 23 byte 1 = byte KEDUA slot 0 = 0xa0 */
	if (r.xtal == 0xa0) {
		printf("  ✅ xtal benar (extended header blok >= 23 terparse)\n");
	} else {
		printf("  ❌ xtal salah (0x%02x, harus 0xa0)\n", r.xtal);
		fails++;
	}

	/* rom[184] = blok 23 byte 0 = byte PERTAMA slot 0 = 0x17 */
	if (r.channel_plan == 0x17) {
		printf("  ✅ channel_plan benar (0x17)\n");
	} else {
		printf("  ❌ channel_plan salah (0x%02x, harus 0x17)\n", r.channel_plan);
		fails++;
	}

	/* rom[215] = blok 26 byte 7 = byte KEDUA slot 3 = 0xa0
	 * rom[216] = blok 27 byte 0 = byte PERTAMA slot 0 = 0x1b */
	{
		uint8_t exp0 = (uint8_t)(marker(26) >> 8);	/* 0xa0 */
		uint8_t exp1 = (uint8_t)(marker(27) & 0xff);	/* 0x1b */
		int ok = (r.mac[0] == exp0) && (r.mac[1] == exp1);

		printf("  %s MAC byte0=0x%02x (harus 0x%02x), byte1=0x%02x (harus 0x%02x)\n",
		    ok ? "✅" : "❌", r.mac[0], exp0, r.mac[1], exp1);
		if (!ok) {
			fails++;
		}
	}

	return fails ? 1 : 0;
}
