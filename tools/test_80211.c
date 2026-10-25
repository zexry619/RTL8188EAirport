/*
 * test_80211.c — uji offline pembangun & parser frame 802.11.
 *
 * Menguji:
 *  1. Probe request: header MAC, element SSID, supported rates, DS param
 *  2. Beacon buatan → parser SSID & BSSID
 *  3. Kasus batas: SSID panjang, buffer kecil, frame terpotong
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtwn_80211.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ✅ %s\n", msg); } \
	else { printf("  ❌ %s\n", msg); fails++; } \
} while (0)

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t src_mac[6] = { 0x98, 0x03, 0x8e, 0x9e, 0xbc, 0x24 };

static void test_probe_request(void)
{
	uint8_t buf[128];
	size_t len = 0;
	char ssid[] = "MyWiFi";

	printf("\n── Probe request ──\n");

	CHECK(rtwn_build_probe_request(bcast, src_mac, ssid, 6, 6, buf,
	    sizeof(buf), &len), "frame terbangun");

	CHECK(len == 24 + 2 + 6 + 2 + 8 + 2 + 1,
	    "panjang frame = 45 (header 24 + SSID 8 + rates 10 + DS 3)");
	CHECK(buf[0] == 0x40 && buf[1] == 0x00, "frame control = 0x0040 (probe request)");
	CHECK(memcmp(buf + 4, bcast, 6) == 0, "addr1 = broadcast");
	CHECK(memcmp(buf + 10, src_mac, 6) == 0, "addr2 = source MAC");
	CHECK(buf[16] == 0xff && buf[21] == 0xff, "addr3 = broadcast");
	CHECK(buf[24] == 0x00 && buf[25] == 0x06, "element id SSID (0x00) + len 6");
	CHECK(memcmp(buf + 26, "MyWiFi", 6) == 0, "isi SSID benar");
	CHECK(buf[32] == 0x01 && buf[33] == 0x08, "element id Supported Rates (0x01) + len 8");
	CHECK(buf[42] == 0x03 && buf[43] == 0x01 && buf[44] == 6,
	    "element DS Parameter Set: channel 6");
}

static void test_probe_request_bounds(void)
{
	uint8_t buf[128];
	size_t len = 0;
	char ssid[33];

	printf("\n── Probe request: kasus batas ──\n");

	/* SSID 32 byte (maks) */
	memset(ssid, 'A', 32);
	CHECK(rtwn_build_probe_request(bcast, src_mac, ssid, 32, 1, buf,
	    sizeof(buf), &len), "SSID 32 byte diterima");
	CHECK(len == 24 + 2 + 32 + 2 + 8 + 2 + 1, "panjang = 71");

	/* SSID 33 byte → harus ditolak */
	CHECK(!rtwn_build_probe_request(bcast, src_mac, ssid, 33, 1, buf,
	    sizeof(buf), &len), "SSID 33 byte DITOLAK");

	/* SSID 0 byte (wildcard probe) */
	CHECK(rtwn_build_probe_request(bcast, src_mac, ssid, 0, 1, buf,
	    sizeof(buf), &len), "SSID kosong (wildcard) diterima");
	CHECK(len == 24 + 2 + 0 + 2 + 8 + 2 + 1, "panjang wildcard = 39");

	/* buffer terlalu kecil → ditolak */
	CHECK(!rtwn_build_probe_request(bcast, src_mac, ssid, 6, 1, buf,
	    40, &len), "buffer kecil DITOLAK");
}

static void test_beacon_parse(void)
{
	uint8_t beacon[128];
	size_t blen = 0;
	char ssid[33];
	uint8_t slen = 0;
	uint8_t bssid[6];
	const char *net = "TokoWiFi";
	size_t el;

	printf("\n── Beacon: parser SSID & BSSID ──\n");

	/*
	 * Beacon nyata: header 24 byte + field tetap 12 byte
	 * (timestamp 8, interval 2, capability 2) + elemen.
	 * Offset 36 inilah yang salah sebelumnya (dulu 24).
	 */
	memset(beacon, 0, sizeof(beacon));
	beacon[0] = 0x80;		/* mgmt / beacon */
	beacon[1] = 0x00;
	memcpy(beacon + 4, bcast, 6);
	memcpy(beacon + 10, src_mac, 6);
	memcpy(beacon + 16, src_mac, 6);	/* BSSID = src */
	beacon[32] = 0x64;		/* beacon interval 100 */
	beacon[34] = 0x31;		/* capability: ESS */
	beacon[35] = 0x04;
	el = 36;
	beacon[el++] = 0x00;		/* SSID element */
	beacon[el++] = (uint8_t)strlen(net);
	memcpy(beacon + el, net, strlen(net));
	el += strlen(net);
	beacon[el++] = 0x01;		/* Supported rates */
	beacon[el++] = 0x04;
	beacon[el++] = 0x82; beacon[el++] = 0x84;
	beacon[el++] = 0x8b; beacon[el++] = 0x96;
	blen = el;

	CHECK(rtwn_parse_ssid(beacon, blen, ssid, &slen), "SSID ditemukan");
	CHECK(slen == strlen(net) && strcmp(ssid, net) == 0, "isi SSID benar");
	CHECK(rtwn_parse_bssid(beacon, blen, bssid), "BSSID diambil");
	CHECK(memcmp(bssid, src_mac, 6) == 0, "BSSID benar");

	/* Probe response: layout fixed 12 byte sama seperti beacon. */
	beacon[0] = 0x50;
	CHECK(rtwn_parse_ssid(beacon, blen, ssid, &slen), "probe response: SSID ditemukan");
	CHECK(slen == strlen(net) && strcmp(ssid, net) == 0,
	    "probe response: isi SSID benar");
}

static void test_probe_request_parse(void)
{
	uint8_t buf[128];
	size_t len = 0;
	char ssid[33];
	uint8_t slen = 0;

	printf("\n── Probe request: parser (tanpa field tetap) ──\n");

	CHECK(rtwn_build_probe_request(bcast, src_mac, "ScanMe", 6, 1, buf,
	    sizeof(buf), &len), "frame terbangun");
	CHECK(rtwn_parse_ssid(buf, len, ssid, &slen),
	    "SSID dibaca dari probe request (body mulai offset 24)");
	CHECK(slen == 6 && strcmp(ssid, "ScanMe") == 0, "isi SSID benar");
}

static void test_parse_edges(void)
{
	uint8_t beacon[96];
	char ssid[33];
	uint8_t slen = 0;

	printf("\n── Beacon: kasus batas ──\n");

	memset(beacon, 0, sizeof(beacon));
	beacon[0] = 0x80;

	/* frame terlalu pendek */
	CHECK(!rtwn_parse_ssid(beacon, 10, ssid, &slen), "frame pendek DITOLAK");

	/* element terpotong: SSID klaim 32 byte, frame cuma 40 */
	beacon[36] = 0x00;
	beacon[37] = 0x20;
	CHECK(!rtwn_parse_ssid(beacon, 40, ssid, &slen),
	    "element terpotong DITOLAK");

	/* bukan frame management → ditolak */
	beacon[0] = 0x08;		/* type = data */
	CHECK(!rtwn_parse_ssid(beacon, 60, ssid, &slen), "frame non-management DITOLAK");

	/* tanpa SSID → ditolak (tidak ada elemen 0x00) */
	beacon[0] = 0x80;
	beacon[36] = 0x01;		/* supported rates, bukan SSID */
	beacon[37] = 0x02;
	beacon[38] = 0x82;
	beacon[39] = 0x84;
	CHECK(!rtwn_parse_ssid(beacon, 40, ssid, &slen), "tanpa SSID → tidak ditemukan");

	/* SSID tersembunyi (length 0) → dianggap valid, panjang 0 */
	beacon[36] = 0x00;
	beacon[37] = 0x00;
	CHECK(rtwn_parse_ssid(beacon, 40, ssid, &slen), "SSID hidden diterima");
	CHECK(slen == 0, "SSID hidden panjang 0");
}

static void test_join_frames(void)
{
	uint8_t buf[256];
	size_t len = 0;
	uint16_t status = 0;
	uint8_t bssid[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	uint8_t other[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	uint16_t cap;

	printf("\n── Auth / Assoc / Null-data ──\n");

	/* ── auth ── */
	CHECK(rtwn_build_auth_req(bssid, src_mac, 0x10, 0, 1, buf,
	    sizeof(buf), &len), "auth terbangun");
	CHECK(len == 30, "auth: panjang 30 (24 + 6 body)");
	CHECK(buf[0] == 0xb0 && buf[1] == 0x00, "auth: FC = 0x00b0");
	CHECK(memcmp(buf + 4, bssid, 6) == 0, "auth: addr1 = BSSID");
	CHECK(memcmp(buf + 10, src_mac, 6) == 0, "auth: addr2 = STA");
	CHECK(buf[22] == 0x00 && buf[23] == 0x01, "auth: seq = 0x10 << 4");
	CHECK(buf[24] == 0 && buf[25] == 0, "auth: algoritma open (0)");
	CHECK(buf[26] == 1 && buf[27] == 0, "auth: auth seq = 1");

	/* ── assoc ── */
	CHECK(rtwn_build_assoc_req(bssid, src_mac, "ZEKRI.NET", 9, 6, false,
	    0x20, buf, sizeof(buf), &len), "assoc terbangun");
	CHECK(buf[0] == 0x00 && buf[1] == 0x00, "assoc: FC = 0x0000");
	cap = (uint16_t)buf[24] | ((uint16_t)buf[25] << 8);
	CHECK((cap & 0x0001) != 0, "assoc: capability ESS");
	CHECK((cap & 0x0010) == 0, "assoc: Privacy mati untuk jaringan terbuka");
	CHECK(buf[26] == 0x0a && buf[27] == 0x00, "assoc: listen interval 10");
	CHECK(buf[28] == 0x00 && buf[29] == 9, "assoc: element SSID len 9");
	CHECK(memcmp(buf + 30, "ZEKRI.NET", 9) == 0, "assoc: SSID benar");
	CHECK(len == 24 + 4 + 11 + 10 + 6 + 3, "assoc: panjang total 58");

	/* Privacy harus mengubah capability */
	CHECK(rtwn_build_assoc_req(bssid, src_mac, "X", 1, 1, true, 0x21, buf,
	    sizeof(buf), &len), "assoc WPA terbangun");
	cap = (uint16_t)buf[24] | ((uint16_t)buf[25] << 8);
	CHECK((cap & 0x0010) != 0, "assoc: Privacy set untuk WPA/WEP");

	/* ── null data ── */
	CHECK(rtwn_build_null_data(bssid, src_mac, false, 0x30, buf,
	    sizeof(buf), &len), "null-data terbangun");
	CHECK(len == 24, "null-data: panjang 24 (tanpa body)");
	CHECK(buf[0] == 0x48 && buf[1] == 0x01, "null-data: FC = 0x0148 (ToDS)");
	CHECK(memcmp(buf + 4, bssid, 6) == 0, "null-data: addr1 = BSSID");

	CHECK(rtwn_build_null_data(bssid, src_mac, true, 0x31, buf,
	    sizeof(buf), &len), "QoS-null terbangun");
	CHECK(len == 26, "QoS-null: panjang 26 (+2 QoS control)");
	CHECK(buf[0] == 0xc8 && buf[1] == 0x01, "QoS-null: FC = 0x01c8");

	/* ── parser respons ── */
	memset(buf, 0, sizeof(buf));
	buf[0] = 0xb0;
	memcpy(buf + 4, src_mac, 6);
	buf[28] = 0x00;
	buf[29] = 0x00;
	CHECK(rtwn_parse_auth_resp(buf, 30, &status) && status == 0,
	    "auth resp: status 0 terbaca");
	buf[28] = 0x25;		/* status 37 */
	CHECK(rtwn_parse_auth_resp(buf, 30, &status) && status == 37,
	    "auth resp: status 37 terbaca");

	memset(buf, 0, sizeof(buf));
	buf[0] = 0x10;
	memcpy(buf + 4, src_mac, 6);
	buf[26] = 0x11;		/* status 17 */
	CHECK(rtwn_parse_assoc_resp(buf, 28, &status) && status == 17,
	    "assoc resp: status 17 terbaca");

	/* ── filter alamat ── */
	CHECK(rtwn_frame_for(buf, 28, src_mac), "frame_for: cocok");
	CHECK(!rtwn_frame_for(buf, 28, other), "frame_for: alamat lain ditolak");

	/* ── batas buffer ── */
	CHECK(!rtwn_build_auth_req(bssid, src_mac, 0, 0, 1, buf, 20, &len),
	    "auth: buffer kecil DITOLAK");
	CHECK(!rtwn_build_null_data(bssid, src_mac, true, 0, buf, 25, &len),
	    "QoS-null: buffer kecil DITOLAK");
	CHECK(!rtwn_parse_assoc_resp(buf, 10, &status),
	    "assoc resp: frame pendek DITOLAK");
}

int main(void)
{
	printf("══ Test frame 802.11 (M2) ══\n");

	test_probe_request();
	test_probe_request_bounds();
	test_beacon_parse();
	test_probe_request_parse();
	test_parse_edges();
	test_join_frames();

	printf("\n%s\n", fails == 0 ? "✅ semua uji lulus" : "❌ ada kegagalan");
	return fails == 0 ? 0 : 1;
}
