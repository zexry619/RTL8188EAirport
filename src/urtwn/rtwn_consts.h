/*
 * rtwn_consts.h — konstanta dari OpenBSD (ISC).
 *
 * Sumber:
 *   sys/dev/ic/rtwnvar.h   → chip flags, flag internal
 *   sys/dev/usb/if_urtwn.c → konstanta halaman/queue per chipset, USB request
 *
 * Nilai TIDAK boleh diubah sembarangan:nilai-nilai ini dipakai langsung oleh hardware.
 */

#ifndef RTL8188EU_RTWN_CONSTS_H
#define RTL8188EU_RTWN_CONSTS_H

/* --- rtwnvar.h --- */
#define RTWN_FLAG_CCK_HIPWR		0x01
#define RTWN_FLAG_BUSY			0x02
#define RTWN_FLAG_FORCE_RAID_11B	0x04
#define RTWN_FLAG_EXT_HDR		0x08

#define RTWN_CHIP_92C			0x00000001
#define RTWN_CHIP_92C_1T2R		0x00000002
#define RTWN_CHIP_UMC			0x00000004
#define RTWN_CHIP_UMC_A_CUT		0x00000008
#define RTWN_CHIP_88C			0x00000010
#define RTWN_CHIP_88E			0x00000020
#define RTWN_CHIP_92E			0x00000040
#define RTWN_CHIP_23A			0x00000080
#define RTWN_CHIP_23B			0x00000100
#define RTWN_CHIP_88F			0x00000200

#define RTWN_CHIP_PCI			0x40000000
#define RTWN_CHIP_USB			0x80000000

/* --- if_urtwn.c --- */
#define R92C_MAX_EPOUT			3

#define R92C_HQ_NPAGES			12
#define R92C_LQ_NPAGES			2
#define R92C_NQ_NPAGES			2
#define R92C_TXPKTBUF_COUNT		256
#define R92C_TX_PAGE_COUNT		248
#define R92C_MAX_RX_DMA_SIZE		0x2800

#define R88E_HQ_NPAGES			12
#define R88E_LQ_NPAGES			0
#define R88E_NQ_NPAGES			13
#define R88E_TXPKTBUF_COUNT		177
#define R88E_TX_PAGE_COUNT		169
#define R88E_MAX_RX_DMA_SIZE		0x2400

#define R88F_HQ_NPAGES			12
#define R88F_LQ_NPAGES			2
#define R88F_NQ_NPAGES			2
#define R88F_TXPKTBUF_COUNT		177
#define R88F_TX_PAGE_COUNT		247
#define R88F_MAX_RX_DMA_SIZE		0x3f80

#define R92E_HQ_NPAGES			16
#define R92E_LQ_NPAGES			16
#define R92E_NQ_NPAGES			16
#define R92E_TX_PAGE_COUNT		248
#define R92E_MAX_RX_DMA_SIZE		0x3fc0

/* Vendor request untuk akses register. */
#define R92C_REQ_REGS			0x05

#endif /* RTL8188EU_RTWN_CONSTS_H */
