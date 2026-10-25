/*
 * rtwn_usb.c — Layer 1: transport USB untuk 8188E (portabel & mockable).
 *
 * Abstraksi ini memisahkan "apa yang harus dikirim" dari "bagaimana mengirimnya",
 * sehingga:
 *   - logika driver bisa diuji tanpa hardware (mock backend)
 *   - backend kernel (IOUSBHostPipe) & backend user space (IOUSBDeviceInterface)
 *     memakai API yang sama
 *
 * Konsep:
 *   - Endpoint bulk OUT (TX): antrean buffer, tiap buffer = 1 frame 802.11
 *     + descriptor 32 byte (r92c_tx_desc_usb) di depan.
 *   - Endpoint bulk IN (RX): chip mengirim descriptor 24 byte (r92c_rx_desc_usb)
 *     + 2 byte SNR/CRC + payload 802.11.
 *
 * Sumber:
 *   - OpenBSD urtwn (if_urtwn.c / r92creg.h) untuk format descriptor
 *   - decompilation vendor (TxFillDescriptor8188EUsb, GetRxPacketShiftBytes8188EUsb)
 */

#include "rtwn_usb.h"
#include <string.h>

#define RTWN_TX_DESC_SIZE 32	/* sizeof(struct r92c_tx_desc_usb) */
#define RTWN_RX_DESC_SIZE 24	/* sizeof(struct r92c_rx_desc_usb) */
#define RTWN_RX_EXTRA_SIZE 2	/* SNR + CRC16 */

/*
 * Bangun descriptor TX 8188E (format OpenBSD r92c_tx_desc_usb, sudah terbukti
 * di chip ini).
 *
 * txdw0: PKTLEN(0-15) | OFFSET(16-23) | OWN | FSG | LSG | BMCAST
 * txdw1: MACID(0-5) | QSEL(8-12) | RAID(13-14) | CCK/…
 * txdw2: AGGBK | CCX_RPT (untuk AMRR)
 */
bool rtwn_usb_build_tx_desc(uint8_t *desc, uint16_t desclen, uint16_t pktlen,
    uint16_t offset, bool isBroadcast, bool isData, uint8_t raid,
    bool useCts, bool useRts, uint8_t ackRate)
{
	uint32_t dw0 = 0, dw1 = 0, dw2 = 0;

	/* Buffer terlalu kecil → jangan tulis apa pun. */
	if (!desc || desclen < RTWN_USB_TX_DESC_SIZE) {
		return false;
	}

	memset(desc, 0, RTWN_TX_DESC_SIZE);

	/*
	 * Bit posisi TEPAT (dari openbsd r92creg.h):
	 *   R92C_TXDW0_OWN    0x80000000
	 *   R92C_TXDW0_FSG    0x08000000
	 *   R92C_TXDW0_LSG    0x04000000
	 *   R92C_TXDW0_BMCAST 0x01000000
	 *   R92C_TXDW0_PKTLEN 0x0000FFFF (bit 0-15)
	 *   R92C_TXDW0_OFFSET 0x00FF0000 (bit 16-23)
	 *   R88E_TXDW1_MACID  0x0000003F (bit 0-5)
	 *   R92C_TXDW1_QSEL   0x00001F00 (bit 8-12)
	 *   R92C_TXDW1_RAID   0x000F0000 (bit 16-19)
	 *   R88E_TXDW2_AGGBK  0x00010000 (bit 16)
	 *   R92C_TXDW2_CCX_RPT 0x00080000 (bit 19)
	 */
	dw0 = (uint32_t)(pktlen & 0xffff);
	dw0 |= (uint32_t)(offset & 0xff) << 16;
	dw0 |= 0x80000000;	/* OWN  */
	dw0 |= 0x08000000;	/* FSG  */
	dw0 |= 0x04000000;	/* LSG  */
	if (isBroadcast) {
		dw0 |= 0x01000000;	/* BMCAST */
	}

	if (isData) {
		dw1 |= (uint32_t)(0x00 & 0x3f);		/* MACID = BSS (0) */
		dw1 |= (uint32_t)(0x00 & 0x1f) << 8;		/* QSEL = BE */
		dw1 |= (uint32_t)(raid & 0x0f) << 16;		/* RAID */
		dw2 |= 0x00010000;	/* AGGBK */
		/* CCX_RPT (0x00080000) dinonaktifkan agar chip tidak membanjiri Bulk-IN */
	}
	(void)useCts;
	(void)useRts;
	(void)ackRate;

	desc[0] = (uint8_t)(dw0 & 0xff);
	desc[1] = (uint8_t)((dw0 >> 8) & 0xff);
	desc[2] = (uint8_t)((dw0 >> 16) & 0xff);
	desc[3] = (uint8_t)((dw0 >> 24) & 0xff);
	desc[4] = (uint8_t)(dw1 & 0xff);
	desc[5] = (uint8_t)((dw1 >> 8) & 0xff);
	desc[6] = (uint8_t)((dw1 >> 16) & 0xff);
	desc[7] = (uint8_t)((dw1 >> 24) & 0xff);
	desc[8] = (uint8_t)(dw2 & 0xff);
	desc[9] = (uint8_t)((dw2 >> 8) & 0xff);
	desc[10] = (uint8_t)((dw2 >> 16) & 0xff);
	desc[11] = (uint8_t)((dw2 >> 24) & 0xff);

	/* Sisa descriptor (32 byte) disetel 0. */
	memset(desc + 12, 0, RTWN_USB_TX_DESC_SIZE - 12);

	return true;
}

/*
 * Bangun descriptor TX untuk frame management.
 *
 * Layout descriptor 32 byte (r92c_tx_desc_usb):
 *   0  txdw0   PKTLEN | OFFSET=32 | OWN | FSG | LSG | BMCAST
 *   4  txdw1   MACID=0 | QSEL=MGNT | RAID=11B
 *   8  txdw2   0
 *   12 txdw3   0
 *   14 txdseq  HWSEQEN (0x8000)
 *   16 txdw4   DRVRATE | HWSEQ
 *   20 txdw5   DATARATE = 0 (CCK 1 Mb/s)
 *   28 txdsum  checksum
 */
bool
rtwn_usb_build_mgmt_tx_desc(uint8_t *desc, uint16_t desclen, uint16_t pktlen,
    bool broadcast)
{
	uint32_t dw0 = 0, dw1 = 0, dw4 = 0;
	uint16_t seq = 0x8000;	/* R92C_TXDW3_HWSEQEN */

	if (!desc || desclen < RTWN_USB_TX_DESC_SIZE) {
		return false;
	}
	memset(desc, 0, RTWN_USB_TX_DESC_SIZE);

	dw0 = (uint32_t)(pktlen & 0xffff);
	dw0 |= (uint32_t)RTWN_USB_TX_DESC_SIZE << 16;	/* OFFSET */
	dw0 |= 0x80000000u;	/* OWN  */
	dw0 |= 0x08000000u;	/* FSG  */
	dw0 |= 0x04000000u;	/* LSG  */
	if (broadcast) {
		dw0 |= 0x01000000u;	/* BMCAST */
	}

	dw1 = (uint32_t)(0x12u << 8);		/* QSEL = MGNT */
	dw1 |= (uint32_t)(0x06u << 16);		/* RAID = 11B */

	/*
	 * Pada RTL8188E (cardType == 0x0f di TxFillDescriptor8188EUsb @0x14d68a),
	 * bit 6 dari txdw4 (0x40) adalah HWSEQ_EN (bukan 0x80 milik 8192C).
	 */
	dw4 = 0x00000100u | 0x00000040u;	/* DRVRATE | R88E_HWSEQ_EN */

	desc[0] = (uint8_t)(dw0 & 0xff);
	desc[1] = (uint8_t)((dw0 >> 8) & 0xff);
	desc[2] = (uint8_t)((dw0 >> 16) & 0xff);
	desc[3] = (uint8_t)((dw0 >> 24) & 0xff);
	desc[4] = (uint8_t)(dw1 & 0xff);
	desc[5] = (uint8_t)((dw1 >> 8) & 0xff);
	desc[6] = (uint8_t)((dw1 >> 16) & 0xff);
	desc[7] = (uint8_t)((dw1 >> 24) & 0xff);
	desc[14] = (uint8_t)(seq & 0xff);
	desc[15] = (uint8_t)((seq >> 8) & 0xff);
	desc[16] = (uint8_t)(dw4 & 0xff);
	desc[17] = (uint8_t)((dw4 >> 8) & 0xff);
	desc[18] = (uint8_t)((dw4 >> 16) & 0xff);
	desc[19] = (uint8_t)((dw4 >> 24) & 0xff);

	rtwn_usb_tx_checksum(desc, RTWN_USB_TX_DESC_SIZE);
	return true;
}

/*
 * Checksum descriptor TX: XOR seluruh 16 word pertama, hasil di word 14.
 * (OpenBSD if_urtwn.c:1683)
 */
void
rtwn_usb_tx_checksum(uint8_t *desc, uint16_t desclen)
{
	uint16_t sum = 0;
	int i;

	if (!desc || desclen < RTWN_USB_TX_DESC_SIZE) {
		return;
	}
	for (i = 0; i < 16; i++) {
		uint16_t w = (uint16_t)desc[i * 2] |
		    ((uint16_t)desc[i * 2 + 1] << 8);
		if (i == 14) {
			w = 0;	/* field checksum sendiri = 0 saat hitung */
		}
		sum ^= w;
	}
	desc[28] = (uint8_t)(sum & 0xff);
	desc[29] = (uint8_t)((sum >> 8) & 0xff);
}

/*
 * Parse descriptor RX 8188E.
 *
 * rxdw0: PKTLEN(0-13) | CRCERR(14) | ICVERR(15) | INFOSZ(16-19) | PHYST(20)
 * rxdw3: RATE(0-4) | …
 * Header = 24 byte descriptor + INFOSZ byte PHY status.
 */
bool rtwn_usb_parse_rx_desc(const uint8_t *buf, uint32_t bufLen,
    rtwn_usb_rxinfo_t *info)
{
	uint32_t dw0, dw3;
	uint32_t pktlen, infosz, totalHdr;
	uint8_t rpt_sel;

	if (bufLen < RTWN_RX_DESC_SIZE) {
		return false;
	}

	dw0 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
	dw3 = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
	    ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24);

	rpt_sel = (uint8_t)((dw3 >> 14) & 0x03);

	memset(info, 0, sizeof(*info));
	info->rate = (uint8_t)(dw3 & 0x3f);

	/*
	 * Sesuai QueryRxDescStatus8188EUsb (@0x14bd80) dan _QueryPacket_8188E
	 * (@0x14c6a0) di RtWlanU.kext:
	 *   rpt_sel == 0: frame 802.11 normal
	 *   rpt_sel == 1: CCX TX Report (selalu 8 byte, tanpa drvinfo/shift)
	 *   rpt_sel == 2: ODM RA TX Report 2 (panjang di dw0 & 0x1ff, tanpa drvinfo/shift)
	 *   rpt_sel == 3: USB HISR / C2H interrupt report (tanpa drvinfo/shift)
	 */
	if (rpt_sel == 1) {
		pktlen = 8;
		infosz = 0;
	} else if (rpt_sel == 2) {
		pktlen = dw0 & 0x01ff;
		infosz = 0;
	} else if (rpt_sel == 3) {
		pktlen = dw0 & 0x3fff;
		infosz = 0;
	} else {
		pktlen = dw0 & 0x3fff;
		infosz = ((dw0 >> 16) & 0x0f) * 8;
		info->crcError = (dw0 & 0x4000) != 0;
		info->icvError = (dw0 & 0x8000) != 0;
		info->shift = (uint8_t)((dw0 >> 24) & 0x03);
		info->hasPhyStatus = (dw0 & 0x04000000) != 0;
	}

	info->pktlen = pktlen;
	info->infosz = infosz;
	totalHdr = RTWN_RX_DESC_SIZE + infosz + info->shift;
	info->headerLen = totalHdr;

	if (totalHdr > bufLen || pktlen > bufLen - totalHdr) {
		return false;
	}
	return true;
}

/*
 * Antrean TX: manage buffer yang siap dikirim.
 * Model: array固定 slot; setiap slot punya buffer + state.
 */
void rtwn_usb_txq_init(rtwn_usb_txq_t *q, rtwn_usb_txbuf_t *slots, uint32_t n)
{
	uint32_t i;

	q->slots = slots;
	q->nslots = n;
	q->head = 0;
	q->tail = 0;
	q->count = 0;
	for (i = 0; i < n; i++) {
		slots[i].used = 0;
		slots[i].len = 0;
		/*
		 * PERBAIKAN (audit 22 Okt 2026): buf & cap sebelumnya TIDAK
		 * diinisialisasi → kalau array slot berada di stack dan tidak
		 * di-zero-kan, isinya garbage.
		 */
		slots[i].buf = NULL;
		slots[i].cap = 0;
	}
}

rtwn_usb_txbuf_t *rtwn_usb_txq_alloc(rtwn_usb_txq_t *q)
{
	rtwn_usb_txbuf_t *s;

	if (q->count >= q->nslots) {
		return NULL;
	}
	s = &q->slots[q->head];
	q->head = (q->head + 1) % q->nslots;
	q->count++;
	s->used = 1;
	return s;
}

bool rtwn_usb_txq_free(rtwn_usb_txq_t *q, rtwn_usb_txbuf_t *s)
{
	uint32_t idx;

	/*
	 * PERBAIKAN KEAMANAN (audit 22 Okt 2026).
	 *
	 * 1. `q->nslots == 0` → `% q->nslots` = PEMBAGIAN DENGAN NOL →
	 *    divide-by-zero trap di kernel = PANIC.
	 * 2. `q->count--` saat count == 0 → wrap ke 0xFFFFFFFF → antrean
	 *    TX mati permanen.
	 * 3. `s - q->slots` untuk pointer dari antrean LAIN = undefined
	 *    behaviour di C.
	 */
	if (!q || !q->slots || q->nslots == 0 || !s || !s->used) {
		return false;
	}
	if (s < q->slots || s >= q->slots + q->nslots) {
		return false;	/* bukan slot dari antrean ini */
	}
	idx = (uint32_t)(s - q->slots);
	/* buffer harus yang paling depan agar urut terkirim */
	if (idx != q->tail) {
		return false;
	}
	q->tail = (q->tail + 1) % q->nslots;
	if (q->count > 0) {
		q->count--;
	}
	s->used = 0;
	s->len = 0;
	return true;
}

uint32_t rtwn_usb_txq_count(const rtwn_usb_txq_t *q)
{
	return q->count;
}
