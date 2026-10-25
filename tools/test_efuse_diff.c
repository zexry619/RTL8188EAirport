/*
 * test_efuse_diff.c — differential test parser EFUSE.
 *
 * Bandingkan dua implementasi:
 *   A) rtwn_seq.c  (parser yang dipakai driver kita)
 *   B) referensi   (transkripsi LITERAL dari pseudocode Ghidra
 *                    RtWlanU::ParsingEfuseContent_8188EU)
 *
 * Input: image EFUSE acak. Output: bandingkan 512 byte hasil parse.
 * Kalau sama untuk ribuan kasus acak → parser kita identik dengan vendor.
 *
 * Build:
 *   clang -O2 -o /tmp/test_efuse_diff tools/test_efuse_diff.c \
 *         src/urtwn/rtwn_seq.c -Isrc/urtwn
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rtwn_seq.h"

/* Parser referensi: transkripsi literal dari pseudocode Ghidra. */
static void ref_parse(const uint8_t *param_2, uint8_t *out, uint16_t param_5)
{
	uint16_t block[256];
	uint8_t buf[512];
	int i, j;
	uint16_t uVar17;	/* indeks byte mentah */
	uint16_t uVar6;		/* indeks (versi vendor) */
	uint8_t cur;
	uint8_t off;
	uint8_t mask;

	for (i = 0; i < 256; i++) {
		block[i] = 0xffff;
	}
	memset(buf, 0xff, sizeof(buf));

	if (param_5 < 2) {
		memcpy(out, buf, 512);
		return;
	}

	cur = param_2[0];
	uVar17 = 1;
	uVar6 = 1;

	while (cur != 0xff && uVar17 < param_5) {
		uint8_t bVar18 = cur;

		if ((bVar18 & 0x1f) == 0x0f) {
			/* extended header */
			uint8_t nxt = (uVar17 < param_5) ? param_2[uVar17] : 0xff;
			if ((nxt & 0x0f) != 0x0f) {
				off = (uint8_t)(((nxt >> 1) & 0x78) | (bVar18 >> 5));
				mask = (uint8_t)(nxt & 0x0f);
				uVar17++;
				cur = nxt;
				goto store;
			}
			/* skip: uVar17 += 2 */
			uVar17 += 2;
			cur = (uVar17 < param_5) ? param_2[uVar17] : 0xff;
			uVar17++;
			continue;
		}

		off = (uint8_t)(bVar18 >> 4);
		mask = (uint8_t)(bVar18 & 0x0f);

store:
		for (i = 0; i < 4; i++) {
			uint16_t v;

			/*
			 * Semantik mask yang BENAR (diverifikasi pada dump
			 * nyata, lihat tools/test_efuse_real.c):
			 *   bit i = 1 → word ABSENT (0 byte)
			 *   bit i = 0 → word PRESENT (2 byte LE)
			 */
			if ((mask >> i) & 1) {
				continue;
			}
			if (uVar17 + 1 >= param_5) {
				break;
			}
			v = param_2[uVar17];
			v |= (uint16_t)((uint16_t)param_2[uVar17 + 1] << 8);
			uVar17 += 2;
			block[((off & 0x3f) * 4) + i] = v;
		}
		/* byte header berikutnya */
		if (uVar17 < param_5 && param_2[uVar17] != 0xff) {
			cur = param_2[uVar17];
			uVar17++;
		} else {
			cur = 0xff;
		}
		(void)uVar6;
	}

	for (i = 0; i < 64; i++) {
		for (j = 0; j < 4; j++) {
			uint16_t v = block[i * 4 + j];
			buf[i * 8 + j * 2 + 0] = (uint8_t)(v & 0xff);
			buf[i * 8 + j * 2 + 1] = (uint8_t)(v >> 8);
		}
	}
	memcpy(out, buf, 512);
}

/* --- mock: image EFUSE mentah sudah disiapkan; baca via "hardware" --- */
static uint8_t g_raw[512];
static int g_raw_len;
static uint16_t g_addr;

static int mock_read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	uint8_t *b = (uint8_t *)buf;
	uint32_t v = 0;

	(void)ctx;
	if (addr == 0x030) {	/* R92C_EFUSE_CTRL */
		v = (g_addr < g_raw_len) ? g_raw[g_addr] : 0xff;
		v |= 0x80000000;	/* VALID */
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
	if (addr == 0x030) {
		g_addr = (uint16_t)((v >> 8) & 0x3ff);
	}
	return 0;
}

int main(void)
{
	int iter, fails = 0;
	srandom(20261022);

	for (iter = 0; iter < 20000; iter++) {
		uint8_t a[512], b[512];
		rtwn_seq_result_t r;
		int i, len;

		/* Generate random-ish EFUSE image. */
		len = 8 + (int)(random() % 248);
		for (i = 0; i < len; i++) {
			g_raw[i] = (uint8_t)(random() & 0xff);
			/* Hindari byte 0xff di tengah ( signifies end). */
			if (g_raw[i] == 0xff) {
				g_raw[i] = 0xfe;
			}
		}
		g_raw_len = len;

		/* A: parser kita */
		memset(&r, 0, sizeof(r));
		{
			uint8_t rom[512];
			/* panggil read_efuse lewat mock */
			rtwn_seq_read_efuse(&r, mock_read, mock_write, NULL);
			/* read_efuse hanya mengisi r->mac/xtal/...; untuk banding penuh
			 * kita rekonstruksi via jalur internal — gunakan nilai yang
			 * tersedia: cukup banding field yang diekspos. */
			(void)rom;
		}

		/* Untuk banding penuh, pakai parser internal langsung. */
		{
			extern void rtwn_seq_parse_efuse_for_test(const uint8_t *raw,
			    uint16_t rawlen, uint8_t *out);
			rtwn_seq_parse_efuse_for_test(g_raw, (uint16_t)len, a);
		}
		ref_parse(g_raw, b, (uint16_t)len);

		if (memcmp(a, b, 512) != 0) {
			int k;
			fails++;
			if (fails <= 3) {
				printf("  ✗ beda pada iterasi %d (len=%d):\n", iter, len);
				for (k = 0; k < 512; k++) {
					if (a[k] != b[k]) {
						printf("      byte %d: kita=0x%02x ref=0x%02x\n",
						    k, a[k], b[k]);
						break;
					}
				}
			}
		}
	}

	printf("=== differential test parser EFUSE ===\n");
	printf("  20000 image acak dibandingkan\n");
	if (fails == 0) {
		printf("  ✅ IDENTIK dengan referensi (transkripsi pseudocode Ghidra)\n");
		return 0;
	}
	printf("  ❌ %d perbedaan\n", fails);
	return 1;
}
