/*
 * rtwn_m1.h — Milestone M1: reduced port dari OpenBSD rtwn/urtwn.
 *
 * Yang sudah diport (dari OpenBSD, lisensi ISC):
 *   - akses register lewat vendor request USB   (if_urtwn.c: urtwn_read/write_region_1)
 *   - baca EFUSE + parse ROM 88E               (rtwn.c: rtwn_efuse_read*, rtwn_r88e_read_rom)
 *   - deteksi chip 88E                         (rtwn.c: rtwn_read_chipid)
 *   - power on 88E                             (if_urtwn.c: urtwn_r88e_power_on)
 *   - inisialisasi DMA                          (if_urtwn.c: urtwn_dma_init)
 *   - upload firmware 8051                      (if_urtwn.c: urtwn_fw_loadpage
 *                                                rtwn.c:     rtwn_load_firmware, rtwn_r88e_fw_reset)
 *
 * Yang BELUM (untuk M2 ke depan): net80211, TX/RX bulk pipe, scan, join, key.
 *
 * Lisensi: GPL-2.0-or-later (lihat ../../LICENSES.md)
 */

#ifndef RTL8188EU_RTWN_M1_H
#define RTL8188EU_RTWN_M1_H

#include <IOKit/IOService.h>
#include "usb_compat.h"

/* Konstanta chip/queue: src/urtwn/rtwn_consts.h (dari OpenBSD, ISC). */
#include "rtwn_consts.h"

/* Panjang image firmware dan alamat mulai tulis. */
#define R92C_FW_PAGE_SIZE	4096

/* Status hasil inisialisasi M1. */
#define RTWN_M1_OK		0
#define RTWN_M1_ERR_USB		1
#define RTWN_M1_ERR_CHIP	2
#define RTWN_M1_ERR_EFUSE	3
#define RTWN_M1_ERR_POWERON	4
#define RTWN_M1_ERR_FIRMWARE	5

typedef struct {
	RtwnUsb		usb;
	uint32_t	chip;
	uint32_t	flags;
	uint8_t		fwcur;
	uint8_t		macaddr[6];
	uint8_t		rom[256];
	uint8_t		xtal;
	uint8_t		regulatory;
	uint8_t		channel_plan;
	int		ntx;		/* jumlah bulk OUT endpoint */
	bool		efuseValid;
	bool		fwLoaded;
	bool		dmaReady;
} rtwn_m1_t;

/* Akses register. */
int	rtwn_m1_write_region(rtwn_m1_t *sc, uint16_t addr, const uint8_t *buf, int len);
int	rtwn_m1_read_region(rtwn_m1_t *sc, uint16_t addr, uint8_t *buf, int len);
void	rtwn_m1_write_1(rtwn_m1_t *sc, uint16_t addr, uint8_t val);
void	rtwn_m1_write_2(rtwn_m1_t *sc, uint16_t addr, uint16_t val);
void	rtwn_m1_write_4(rtwn_m1_t *sc, uint16_t addr, uint32_t val);
uint8_t rtwn_m1_read_1(rtwn_m1_t *sc, uint16_t addr);
uint16_t rtwn_m1_read_2(rtwn_m1_t *sc, uint16_t addr);
uint32_t rtwn_m1_read_4(rtwn_m1_t *sc, uint16_t addr);

/* Tahap inisialisasi. */
int	rtwn_m1_read_chipid(rtwn_m1_t *sc);
int	rtwn_m1_read_rom(rtwn_m1_t *sc);
int	rtwn_m1_power_on(rtwn_m1_t *sc);
int	rtwn_m1_dma_init(rtwn_m1_t *sc);
int	rtwn_m1_load_firmware(rtwn_m1_t *sc);

/* Firmware 8188E (dari linux-firmware), disematkan sebagai array C. */
extern const unsigned char rtl8188eufw_bin[];
extern const unsigned int rtl8188eufw_bin_len;

#endif /* RTL8188EU_RTWN_M1_H */
