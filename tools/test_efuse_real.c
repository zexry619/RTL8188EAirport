/*
 * test_efuse_real.c — GOLDEN TEST parser EFUSE dengan dump NYATA dari chip.
 *
 * Dump diambil 22 Okt 2026 dari TP-Link TL-WN725N (RTL8188EUS, 2357:010c)
 * melalui jalur direct (register 0x031/32/33/30) dari USER SPACE.
 *
 * Ini adalah sumber kebenaran TERKUAT: tiga field independen yang sudah
 * diketahui sebelumnya harus muncul di offset yang benar.
 *
 *   offset 0   : 0x29 0x81        → EEPROM ID 0x8129
 *   offset 208 : 0x57 0x23 0x0c 0x01 → VID 0x2357, PID 0x010c (USB)
 *   offset 215 : 98:03:8e:9e:bc:24   → MAC address (dibaca driver vendor)
 *   offset 223 : "Realtek"
 *   offset 232 : "802.11n "
 *
 * Kalau test ini lulus, parser tidak mungkin "hanya cocok dengan dirinya
 * sendiri" (kesalahan yang terjadi pada tools/test_efuse_diff.c sebelum
 * 22 Okt 2026 — referensinya transkripsi pseudocode yang sama-sama salah).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rtwn_seq.h"

static int fails;

#define CHECK(cond, msg) do {						\
	if (cond) {							\
		printf("  ✅ %s\n", msg);				\
	} else {							\
		printf("  ❌ %s\n", msg);				\
		fails++;						\
	}								\
} while (0)

/* 256 byte EFUSE mentah dari chip (jalur direct 0x031/32/33/30). */
static const char real_dump_hex[] =
	"0e29816f33ff000000cf4081ae962d03939611ef48fc8c00119bff01006c0b00"
	"000010000c000000000000202d2d2d2d2d2d2d2d3c2d2d2df1ef20a1201a0000"
	"0000000f3000010010000000002f3e00034f3057230c01436600986f30038e9e"
	"bc240903528f3065616c74656b0d03af303830322e31316e20cf304e49430c03"
	"303045ef303034433030303100202f2e2e2e2e2e33323c323131f1ef2ca12f15"
	"00";


int main(void)
{
	uint8_t rom[512];
	static const uint8_t mac[6] = { 0x98, 0x03, 0x8e, 0x9e, 0xbc, 0x24 };
	static const uint8_t vidpid[4] = { 0x57, 0x23, 0x0c, 0x01 };

	printf("══ Golden test parser EFUSE (dump nyata 8188E) ══\n");

	uint8_t raw[256];
	uint16_t n = (uint16_t)(strlen(real_dump_hex) / 2);

	memset(raw, 0xff, sizeof(raw));
	for (uint16_t i = 0; i < n && i < sizeof(raw); i++) {
		unsigned v;

		if (sscanf(real_dump_hex + i * 2, "%2x", &v) != 1)
			break;
		raw[i] = (uint8_t)v;
	}
	printf("  (info) dump %u byte\n", n);

	memset(rom, 0, sizeof(rom));
	rtwn_seq_efuse_parse(raw, sizeof(raw), rom);

	CHECK(rom[0] == 0x29 && rom[1] == 0x81,
	    "EEPROM ID = 0x8129 di offset 0");
	CHECK(memcmp(rom + 208, vidpid, 4) == 0,
	    "VID/PID 2357:010c di offset 208");
	CHECK(memcmp(rom + 215, mac, 6) == 0,
	    "MAC 98:03:8e:9e:bc:24 di offset 215");
	CHECK(memcmp(rom + 223, "Realtek", 7) == 0,
	    "string \"Realtek\" di offset 223");
	CHECK(memcmp(rom + 232, "802.11n", 7) == 0,
	    "string \"802.11n \" di offset 232");

	printf("  (info) channel_plan=0x%02x xtal=0x%02x rf_board_opt=0x%02x\n",
	    rom[184], rom[185], rom[193]);

	/* Mask semantics: bit SET = word absent. Dump ini punya blok yang
	 * ditulis dua kali (patch di akhir stream) — nilai terakhir menang. */
	CHECK(rom[184] == 0xa1, "blok 23 patch terakhir menang (channel_plan)");

	printf("\n%s\n", fails == 0 ? "✅ semua uji lulus" : "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
