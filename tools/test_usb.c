/*
 * test_usb.c — uji lapisan transport USB tanpa hardware.
 *
 * Memverifikasi:
 *   1. Descriptor TX: bit-field sesuai definisi OpenBSD r92c_tx_desc_usb
 *   2. Descriptor RX: parsing PKTLEN / RATE / PHYST / error bits
 *   3. Ring TX: alokasi, batas penuh, urutan free (FIFO), guard double-free
 */

#include <stdio.h>
#include <string.h>
#include "rtwn_usb.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_tx_desc(void)
{
	uint8_t d[RTWN_USB_TX_DESC_SIZE];
	uint32_t dw0, dw1, dw2;

	printf("── Descriptor TX (r92c_tx_desc_usb) ──\n");

	/* frame data 100 byte, broadcast, RAID=11b (0) */
	rtwn_usb_build_tx_desc(d, sizeof(d), 100, RTWN_USB_TX_DESC_SIZE, true, true, 0, false, false, 0);

	dw0 = rd32(d);
	dw1 = rd32(d + 4);
	dw2 = rd32(d + 8);

	printf("     dw0=0x%08x dw1=0x%08x dw2=0x%08x\n", dw0, dw1, dw2);

	CHECK((dw0 & 0xffff) == 100, "PKTLEN = 100 (bits 0-15)");
	CHECK(((dw0 >> 16) & 0xff) == RTWN_USB_TX_DESC_SIZE,
	    "OFFSET = 32 (bits 16-23) → frame 802.11 mulai di byte 32");
	CHECK((dw0 & 0x80000000) == 0x80000000, "bit OWN (0x80000000) diset");
	CHECK((dw0 & 0x08000000) == 0x08000000, "bit FSG (0x08000000) diset");
	CHECK((dw0 & 0x04000000) == 0x04000000, "bit LSG (0x04000000) diset");
	CHECK((dw0 & 0x01000000) == 0x01000000, "bit BMCAST (0x01000000) diset");
	CHECK((dw1 & 0x3f) == 0, "MACID = 0 (BSS)");
	CHECK(((dw1 >> 8) & 0x1f) == 0, "QSEL = 0 (BE)");
	CHECK(((dw1 >> 16) & 0x0f) == 0, "RAID = 0 (11BG)");
	CHECK((dw2 & 0x00010000) == 0x00010000, "AGGBK (0x10000) diset");
	CHECK((dw2 & 0x00080000) == 0x00080000, "CCX_RPT (0x80000) diset (TX report AMRR)");

	/* frame management (bukan data) → tanpa AGGBK/RAID */
	rtwn_usb_build_tx_desc(d, sizeof(d), 24, RTWN_USB_TX_DESC_SIZE, false, false, 0, false, false, 0);
	dw2 = rd32(d + 8);
	CHECK((dw2 & 0x00090000) == 0, "frame management: tanpa AGGBK/CCX_RPT");
}

static void test_rx_desc(void)
{
	/*
	 * Buffer RX di dunia nyata = descriptor (26 B) + payload frame.
	 * PKTLEN di descriptor HANYA valid kalau payload-nya benar-benar
	 * ada di buffer — kalau tidak, parser harus menolak (lihat test
	 * "PKTLEN nakal ditolak" di bawah).
	 */
	uint8_t buf[26 + 64];
	rtwn_usb_rxinfo_t info;

	printf("\n── Descriptor RX (r92c_rx_desc_usb) ──\n");

	memset(buf, 0, sizeof(buf));
	/* dw0: PKTLEN=64, INFOSZ=2 (16 byte PHY status), PHYST set
	 *     INFOSZ di bit 16-19, PHYST di bit 26 (0x04000000). */
	buf[0] = 64;
	buf[1] = 0;
	buf[2] = (uint8_t)(2 << 0);	/* INFOSZ=2 → 16 byte */
	buf[3] = 0x04;		/* PHYST = bit 26 (0x04000000) */
	buf[4] = 0;
	/* dw3: RATE=4 */
	buf[12] = 4;

	CHECK(rtwn_usb_parse_rx_desc(buf, sizeof(buf), &info), "parse berhasil");
	CHECK(info.pktlen == 64, "PKTLEN = 64");
	CHECK(info.infosz == 16, "INFOSZ = 16 byte (2 × 8)");
	CHECK(info.rate == 4, "RATE = 4");
	CHECK(info.hasPhyStatus, "PHYST terdeteksi");
	CHECK(info.headerLen == 26, "header total = 24 + 2 byte");
	CHECK(!info.crcError && !info.icvError, "tidak ada error flag");

	/* error flag */
	buf[0] = 64 | 0x40;	/* ubah: CRCERR = bit 14 (0x4000) */
	buf[0] = 64;
	buf[1] = 0x40;		/* CRCERR di bit 14 */
	CHECK(rtwn_usb_parse_rx_desc(buf, sizeof(buf), &info) && info.crcError,
	    "CRCERR terdeteksi");

	/* buffer terlalu pendek */
	CHECK(!rtwn_usb_parse_rx_desc(buf, 10, &info), "buffer pendek ditolak");

	/*
	 * ‑‑‑ PERBAIKAN KEAMANAN (audit 22 Okt 2026) ‑‑‑
	 *
	 * PKTLEN = 14 bit → chip bisa mengklaim frame sepanjang 16383
	 * byte padahal hanya 38 byte yang benar-benar diterima. Tanpa
	 * validasi, lapisan driver akan membaca ~16 KB di luar buffer
	 * kernel 2 KB → kernel panic.
	 */
	{
		uint8_t evil[26 + 64];
		memset(evil, 0, sizeof(evil));
		evil[0] = 0xff;		/* PKTLEN = 16383 */
		evil[1] = 0x3f;
		evil[12] = 4;
		CHECK(!rtwn_usb_parse_rx_desc(evil, sizeof(evil), &info),
		    "PKTLEN nakal (16383 > buffer) ditolak");
	}
	{
		uint8_t tiny[26];
		memset(tiny, 0, sizeof(tiny));
		tiny[0] = 0x10;		/* PKTLEN = 16, tapi payload 0 byte */
		CHECK(!rtwn_usb_parse_rx_desc(tiny, sizeof(tiny), &info),
		    "PKTLEN melebihi payload ditolak");
	}
}

static void test_txq(void)
{
	rtwn_usb_txbuf_t slots[4];
	rtwn_usb_txq_t q;
	rtwn_usb_txbuf_t *a, *b, *c;

	printf("\n── Ring TX ──\n");

	rtwn_usb_txq_init(&q, slots, 4);
	CHECK(rtwn_usb_txq_count(&q) == 0, "ring kosong");
	CHECK(rtwn_usb_txq_alloc(&q) == &slots[0], "alokasi pertama → slot 0");
	a = rtwn_usb_txq_alloc(&q);
	CHECK(a == &slots[1], "alokasi kedua → slot 1");
	b = rtwn_usb_txq_alloc(&q);
	CHECK(b == &slots[2], "alokasi ketiga → slot 2");
	c = rtwn_usb_txq_alloc(&q);
	CHECK(c == &slots[3], "alokasi keempat → slot 3");
	CHECK(rtwn_usb_txq_alloc(&q) == NULL, "ring penuh → alokasi gagal");
	CHECK(rtwn_usb_txq_count(&q) == 4, "count = 4");

	CHECK(rtwn_usb_txq_free(&q, &slots[0]), "free slot 0 (FIFO head) OK");
	CHECK(!rtwn_usb_txq_free(&q, &slots[0]), "double-free ditolak");
	CHECK(!rtwn_usb_txq_free(&q, &slots[2]), "free out-of-order ditolak");
	CHECK(rtwn_usb_txq_alloc(&q) == &slots[0], "slot 0 dipakai ulang");
	CHECK(rtwn_usb_txq_count(&q) == 4, "count tetap 4");
}

int main(void)
{
	printf("=== uji lapisan transport USB 8188E (tanpa hardware) ===\n\n");
	test_tx_desc();
	test_rx_desc();
	test_txq();
	printf("\n%s\n", fails ? "❌ ada kegagalan" : "✅ semua uji lulus");
	return fails ? 1 : 0;
}
