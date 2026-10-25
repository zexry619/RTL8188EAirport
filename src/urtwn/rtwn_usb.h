/*
 * rtwn_usb.h — Layer 1: transport USB 8188E (portabel & mockable).
 */

#ifndef RTL8188EU_RTWN_USB_H
#ifdef __cplusplus
extern "C" {
#endif

#define RTL8188EU_RTWN_USB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RTWN_USB_TX_DESC_SIZE 32
#define RTWN_USB_RX_DESC_SIZE 24
#define RTWN_USB_RX_EXTRA_SIZE 2

/* Info hasil parse descriptor RX. */
typedef struct {
	uint32_t pktlen;
	uint32_t infosz;	/* panjang PHY status, kelipatan 8 */
	uint8_t  rate;
	bool     crcError;
	bool     icvError;
	bool     hasPhyStatus;
	uint8_t  shift;		/* 2-bit alignment shift (0..3) */
	uint32_t headerLen;	/* 26 */
} rtwn_usb_rxinfo_t;

/* Slot buffer TX. */
typedef struct {
	uint8_t *buf;
	uint32_t cap;
	uint32_t len;
	bool     used;
} rtwn_usb_txbuf_t;

/* Antrean TX (ring). */
typedef struct {
	rtwn_usb_txbuf_t *slots;
	uint32_t nslots;
	uint32_t head;
	uint32_t tail;
	uint32_t count;
} rtwn_usb_txq_t;

/*
 * Build descriptor TX 8188E (format OpenBSD r92c_tx_desc_usb).
 *
 * `desclen` = ukuran buffer `desc` yang sebenarnya. Fungsi ini menulis
 * RTWN_USB_TX_DESC_SIZE (32) byte → kalau buffer lebih kecil, penulisan
 * melewati batas (audit 22 Okt 2026). Nilai balik false = buffer
 * terlalu kecil, TIDAK ada yang ditulis.
 */
bool rtwn_usb_build_tx_desc(uint8_t *desc, uint16_t desclen, uint16_t pktlen,
    uint16_t offset, bool isBroadcast, bool isData, uint8_t raid,
    bool useCts, bool useRts, uint8_t ackRate);

/*
 * Bangun descriptor TX untuk frame MANAGEMENT (probe request, auth, ...).
 *
 * Mengikuti urtwn_tx_fill_desc() cabang non-data:
 *   - QSEL = MGNT (0x12), RAID = 11B (6), MACID = 0
 *   - DRVRATE + DATARATE CCK1 (0) — paling andal untuk scan
 *   - HWSEQ (nomor urut diisi hardware)
 *
 * Checksum descriptor dihitung dan ditulis ke word 14 (byte 28).
 */
bool rtwn_usb_build_mgmt_tx_desc(uint8_t *desc, uint16_t desclen,
    uint16_t pktlen, bool broadcast);

/* Hitung checksum descriptor USB (XOR 16 word pertama, simpan di word 14). */
void rtwn_usb_tx_checksum(uint8_t *desc, uint16_t desclen);


/* Parse descriptor RX 8188E. */
bool rtwn_usb_parse_rx_desc(const uint8_t *buf, uint32_t bufLen,
    rtwn_usb_rxinfo_t *info);

/* Manajemen ring TX. */
void rtwn_usb_txq_init(rtwn_usb_txq_t *q, rtwn_usb_txbuf_t *slots, uint32_t n);
rtwn_usb_txbuf_t *rtwn_usb_txq_alloc(rtwn_usb_txq_t *q);
bool rtwn_usb_txq_free(rtwn_usb_txq_t *q, rtwn_usb_txbuf_t *s);
uint32_t rtwn_usb_txq_count(const rtwn_usb_txq_t *q);

#ifdef __cplusplus
}
#endif

#endif /* RTL8188EU_RTWN_USB_H */
