/*
 * rtwn_80211.c — pembangun & parser frame 802.11 (bagian M2).
 *
 * Untuk scan, kita butuh:
 *   - membangun Probe Request (management frame, type 0x40) berisi SSID
 *   - mem-parsing Beacon/Probe Response untuk ambil SSID & BSSID
 *
 * Format frame 802.11 (header MAC):
 *   offset 0 : frame control (2 byte, little endian)
 *   offset 2 : duration (2 byte)
 *   offset 4 : address 1 (dest)      6 byte
 *   offset 10: address 2 (source)     6 byte
 *   offset 16: address 3 (BSSID)      6 byte
 *   offset 22: sequence control (2 byte)
 *   offset 24: [frame body]
 *
 * BIT DI BODY:
 *   +0  : element id
 *   +1  : length
 *   +2..: data
 *
 * Frame control untuk Probe Request:
 *   type 0b10 (management), subtype 0b0000 (probe request)
 *   → FC = 0x0040  (little endian: 0x40, 0x00)
 */

#include "rtwn_80211.h"

#include <string.h>

/*
 * Bangun Probe Request.
 *
 * @dst, @src : alamat MAC 6 byte
 * @ssid      : nama jaringan (maks 32 byte)
 * @ssid_len  : panjang SSID
 * @out       : buffer keluaran (min 40 byte)
 * @out_len   : panjang frame yang dihasilkan
 *
 * Body yang ditambahkan:
 *   - SSID element (id 0x00)
 *   - Supported rates (id 0x01): 1, 2, 5.5, 11, 18, 24, 36, 54 Mb/s
 *   - DS parameter set (id 0x03): channel 1
 *
 * Return true kalau berhasil.
 */
bool
rtwn_build_probe_request(const uint8_t dst[6], const uint8_t src[6],
    const char *ssid, uint8_t ssid_len, uint8_t chan, uint8_t *out,
    size_t out_cap, size_t *out_len)
{
	size_t n = 0;
	size_t body_start;

	if (!dst || !src || !ssid || !out || !out_len) {
		return false;
	}
	if (ssid_len > 32) {
		return false;
	}
	/* header 24 + SSID(2+32) + rates(2+8) + DS(2+1) = 71 */
	if (out_cap < 72) {
		return false;
	}

	memset(out, 0, out_cap);

	/* ── header MAC ── */
	out[n++] = 0x40;		/* frame control: mgmt / probe request */
	out[n++] = 0x00;
	out[n++] = 0x00;		/* duration */
	out[n++] = 0x00;
	memcpy(out + n, dst, 6);	n += 6;	/* addr 1 = broadcast */
	memcpy(out + n, src, 6);	n += 6;	/* addr 2 = source */
	memset(out + n, 0xff, 6);	n += 6;	/* addr 3 = broadcast */
	out[n++] = 0x00;		/* seq control hi */
	out[n++] = 0x00;		/* seq control lo */

	body_start = n;

	/* ── SSID element ── */
	out[n++] = 0x00;		/* element id: SSID */
	out[n++] = ssid_len;
	if (ssid_len) {
		memcpy(out + n, ssid, ssid_len);
		n += ssid_len;
	}

	/* ── Supported rates ── */
	out[n++] = 0x01;		/* element id: supported rates */
	out[n++] = 0x08;
	{
		static const uint8_t rates[8] = {
			0x82,	/* 1 Mb/s  (basic)   */
			0x84,	/* 2 Mb/s             */
			0x8b,	/* 5.5 Mb/s           */
			0x96,	/* 11 Mb/s            */
			0x24,	/* 18 Mb/s            */
			0x30,	/* 24 Mb/s            */
			0x48,	/* 36 Mb/s            */
			0x6c	/* 54 Mb/s            */
		};
		memcpy(out + n, rates, sizeof(rates));
		n += sizeof(rates);
	}

	/* ── DS parameter set (channel) ── */
	out[n++] = 0x03;		/* element id: DS parameter set */
	out[n++] = 0x01;
	out[n++] = chan;

	/* isi sisa body dengan 0 (padding) */
	(void)body_start;

	*out_len = n;
	return true;
}

/*
 * Ambil SSID dari frame beacon / probe response.
 *
 * @frame     : pointer ke frame 802.11 (header MAC mulai dari byte 0)
 * @frame_len : panjang frame
 * @ssid_out  : buffer keluaran (min 33 byte)
 * @ssid_len  : panjang SSID yang ditemukan
 *
 * Return true kalau SSID ditemukan.
 */
bool
rtwn_parse_ssid(const uint8_t *frame, size_t frame_len, char *ssid_out,
    uint8_t *ssid_len)
{
	size_t n;
	size_t end;
	uint8_t subtype;

	if (!frame || !ssid_out || !ssid_len) {
		return false;
	}
	if (frame_len < 24) {
		return false;
	}

	/* Frame control: type = management (bits 2..3 == 0) */
	if (((frame[0] >> 2) & 0x03) != 0) {
		return false;
	}

	/*
	 * Beacon (0x80) dan Probe Response (0x50) punya 12 byte field
	 * tetap (timestamp 8 + interval 2 + capability 2) sebelum elemen.
	 * Probe Request (0x40) dan frame mgmt lain langsung elemen.
	 */
	subtype = frame[0] & 0xf0;
	if (subtype == 0x80 || subtype == 0x50) {
		n = 24 + 12;
	} else {
		n = 24;
	}

	end = frame_len;

	while (n + 2 <= end) {
		uint8_t id = frame[n];
		uint8_t len = frame[n + 1];

		if (n + 2 + len > end) {
			break;		/* element terpotong → berhenti */
		}
		if (id == 0x00) {		/* SSID */
			if (len > 32) {
				return false;
			}
			memcpy(ssid_out, frame + n + 2, len);
			ssid_out[len] = 0;
			*ssid_len = len;
			return true;
		}
		n += 2 + len;
	}
	return false;
}

/*
 * Ambil BSSID (address 3) dari frame beacon/probe response.
 */
bool
rtwn_parse_bssid(const uint8_t *frame, size_t frame_len, uint8_t bssid[6])
{
	if (!frame || !bssid) {
		return false;
	}
	if (frame_len < 24) {
		return false;
	}
	memcpy(bssid, frame + 16, 6);
	return true;
}

/* ── M3: auth / assoc / null-data ───────────────────────────────────── */

/*
 * Header 24 byte untuk frame management (auth/assoc).
 * Tidak memakai ToDS — frame mgmt selalu dikirim "apa adanya".
 */
static size_t
put_mgmt_hdr(uint8_t *out, uint8_t fc0, const uint8_t bssid[6],
    const uint8_t src[6], uint16_t seq)
{
	out[0] = fc0;
	out[1] = 0x00;			/* flags: tidak ada ToDS/FromDS */
	out[2] = 0x00;			/* duration */
	out[3] = 0x00;
	memcpy(out + 4, bssid, 6);	/* addr1 = DA */
	memcpy(out + 10, src, 6);	/* addr2 = SA */
	memcpy(out + 16, bssid, 6);	/* addr3 = BSSID */
	out[22] = (uint8_t)((seq << 4) & 0xff);
	out[23] = (uint8_t)(((seq << 4) >> 8) & 0xff);
	return 24;
}

bool
rtwn_build_auth_req(const uint8_t bssid[6], const uint8_t src[6],
    uint16_t seq, uint16_t algo, uint16_t auth_seq, uint8_t *out,
    size_t out_cap, size_t *out_len)
{
	size_t n;

	if (!bssid || !src || !out || !out_len || out_cap < 30) {
		return false;
	}
	memset(out, 0, out_cap);
	n = put_mgmt_hdr(out, 0xb0, bssid, src, seq);

	/* body: algorithm(2) | auth seq(2) | status(2) */
	out[n++] = (uint8_t)(algo & 0xff);
	out[n++] = (uint8_t)(algo >> 8);
	out[n++] = (uint8_t)(auth_seq & 0xff);
	out[n++] = (uint8_t)(auth_seq >> 8);
	out[n++] = 0x00;
	out[n++] = 0x00;

	*out_len = n;
	return true;
}

bool
rtwn_build_assoc_req(const uint8_t bssid[6], const uint8_t src[6],
    const char *ssid, uint8_t ssid_len, uint8_t chan, bool privacy,
    uint16_t seq, uint8_t *out, size_t out_cap, size_t *out_len)
{
	size_t n;
	uint16_t cap;
	static const uint8_t rates[8] = {
		0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c
	};
	static const uint8_t xrates[4] = { 0x0c, 0x12, 0x18, 0x60 };

	if (!bssid || !src || !ssid || !out || !out_len) {
		return false;
	}
	if (ssid_len > 32 || out_cap < 24 + 4 + 2 + 32 + 2 + 8 + 2 + 4 + 3) {
		return false;
	}
	memset(out, 0, out_cap);
	n = put_mgmt_hdr(out, 0x00, bssid, src, seq);

	/* capability: ESS | ShortPreamble | ShortSlot (+ Privacy kalau WPA/WEP) */
	cap = 0x0421;
	if (privacy) {
		cap |= 0x0010;
	}
	out[n++] = (uint8_t)(cap & 0xff);
	out[n++] = (uint8_t)(cap >> 8);
	out[n++] = 0x0a;		/* listen interval = 10 */
	out[n++] = 0x00;

	/* SSID */
	out[n++] = 0x00;
	out[n++] = ssid_len;
	if (ssid_len) {
		memcpy(out + n, ssid, ssid_len);
		n += ssid_len;
	}

	/* Supported rates */
	out[n++] = 0x01;
	out[n++] = sizeof(rates);
	memcpy(out + n, rates, sizeof(rates));
	n += sizeof(rates);

	/* Extended supported rates */
	out[n++] = 0x32;
	out[n++] = sizeof(xrates);
	memcpy(out + n, xrates, sizeof(xrates));
	n += sizeof(xrates);

	/* DS parameter set */
	out[n++] = 0x03;
	out[n++] = 0x01;
	out[n++] = chan;

	*out_len = n;
	return true;
}

bool
rtwn_build_null_data(const uint8_t bssid[6], const uint8_t src[6], bool qos,
    uint16_t seq, uint8_t *out, size_t out_cap, size_t *out_len)
{
	size_t n;

	if (!bssid || !src || !out || !out_len || out_cap < 26) {
		return false;
	}
	memset(out, 0, out_cap);

	out[0] = qos ? 0xc8 : 0x48;	/* data / QoS-null */
	out[1] = 0x01;			/* ToDS */
	out[2] = 0x00;
	out[3] = 0x00;
	memcpy(out + 4, bssid, 6);	/* addr1 = BSSID */
	memcpy(out + 10, src, 6);	/* addr2 = SA */
	memcpy(out + 16, bssid, 6);	/* addr3 = BSSID */
	out[22] = (uint8_t)((seq << 4) & 0xff);
	out[23] = (uint8_t)(((seq << 4) >> 8) & 0xff);
	n = 24;
	if (qos) {
		out[n++] = 0x00;	/* TID 0, no ack policy */
		out[n++] = 0x00;
	}

	*out_len = n;
	return true;
}

bool
rtwn_parse_auth_resp(const uint8_t *frame, size_t frame_len, uint16_t *status)
{
	if (!frame || frame_len < 30) {
		return false;
	}
	if ((frame[0] & 0xf0) != 0xb0) {
		return false;
	}
	if (status) {
		*status = (uint16_t)frame[28] | ((uint16_t)frame[29] << 8);
	}
	return true;
}

bool
rtwn_parse_assoc_resp(const uint8_t *frame, size_t frame_len, uint16_t *status)
{
	if (!frame || frame_len < 28) {
		return false;
	}
	if ((frame[0] & 0xf0) != 0x10) {
		return false;
	}
	if (status) {
		*status = (uint16_t)frame[26] | ((uint16_t)frame[27] << 8);
	}
	return true;
}

bool
rtwn_frame_for(const uint8_t *frame, size_t frame_len, const uint8_t sta[6])
{
	if (!frame || !sta || frame_len < 24) {
		return false;
	}
	return memcmp(frame + 4, sta, 6) == 0;
}

