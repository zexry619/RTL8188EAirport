/*
 * rtwn_m1.cpp — Milestone M1: reduced port dari OpenBSD rtwn/urtwn.
 *
 * Setiap fungsi diberi komentar rujukan ke file upstream OpenBSD agar mudah
 * dibandingkan. Logika dipertahankan sedekat mungkin; yang berubah hanya
 * mekanisme I/O (USB vendor request via IOKit), delay (IODelay), dan struktur
 * data (tidak ada net80211 pada M1).
 */

#include "rtwn_m1.h"
#include "r92creg.h"

#include <stddef.h>
#include <string.h>

#define LOGP "RTL8188EU: "

/* OpenBSD RTWN_DELAY() = delay dalam mikrodetik. */
#define RTWN_DELAY(us) IODelay(us)

/* OpenBSD MS()/SM()/RW() (r92creg.h). */
#ifndef MS
#define MS(val, field)	(((val) & field##_M) >> field##_S)
#endif
#ifndef SM
#define SM(field, val)	(((val) << field##_S) & field##_M)
#endif
#ifndef RW
#define RW(var, field, val)	(((var) & ~field##_M) | SM(field, val))
#endif

/* ------------------------------------------------------------------ */
/* Akses register (OpenBSD: if_urtwn.c urtwn_read_region_1/write_region_1) */
/* ------------------------------------------------------------------ */

int
rtwn_m1_write_region(rtwn_m1_t *sc, uint16_t addr, const uint8_t *buf, int len)
{
	return sc->usb.controlOut(R92C_REQ_REGS, addr, buf, (uint32_t)len);
}

int
rtwn_m1_read_region(rtwn_m1_t *sc, uint16_t addr, uint8_t *buf, int len)
{
	return sc->usb.controlIn(R92C_REQ_REGS, addr, buf, (uint32_t)len);
}

void
rtwn_m1_write_1(rtwn_m1_t *sc, uint16_t addr, uint8_t val)
{
	rtwn_m1_write_region(sc, addr, &val, 1);
}

void
rtwn_m1_write_2(rtwn_m1_t *sc, uint16_t addr, uint16_t val)
{
	uint8_t buf[2];

	buf[0] = (uint8_t)(val & 0xff);
	buf[1] = (uint8_t)(val >> 8);
	rtwn_m1_write_region(sc, addr, buf, 2);
}

void
rtwn_m1_write_4(rtwn_m1_t *sc, uint16_t addr, uint32_t val)
{
	uint8_t buf[4];

	buf[0] = (uint8_t)(val & 0xff);
	buf[1] = (uint8_t)((val >> 8) & 0xff);
	buf[2] = (uint8_t)((val >> 16) & 0xff);
	buf[3] = (uint8_t)((val >> 24) & 0xff);
	rtwn_m1_write_region(sc, addr, buf, 4);
}

uint8_t
rtwn_m1_read_1(rtwn_m1_t *sc, uint16_t addr)
{
	uint8_t val = 0xff;

	if (rtwn_m1_read_region(sc, addr, &val, 1) != 0) {
		return 0xff;
	}
	return val;
}

uint16_t
rtwn_m1_read_2(rtwn_m1_t *sc, uint16_t addr)
{
	uint8_t buf[2] = { 0xff, 0xff };

	if (rtwn_m1_read_region(sc, addr, buf, 2) != 0) {
		return 0xffff;
	}
	return (uint16_t)(buf[0] | (buf[1] << 8));
}

uint32_t
rtwn_m1_read_4(rtwn_m1_t *sc, uint16_t addr)
{
	uint8_t buf[4] = { 0xff, 0xff, 0xff, 0xff };

	if (rtwn_m1_read_region(sc, addr, buf, 4) != 0) {
		return 0xffffffff;
	}
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* ------------------------------------------------------------------ */
/* EFUSE (OpenBSD: rtwn.c rtwn_efuse_read_1 / rtwn_efuse_read)        */
/* ------------------------------------------------------------------ */

static uint8_t
rtwn_m1_efuse_read_1(rtwn_m1_t *sc, uint16_t addr)
{
	uint32_t reg;
	int ntries;

	reg = rtwn_m1_read_4(sc, R92C_EFUSE_CTRL);
	reg = RW(reg, R92C_EFUSE_CTRL_ADDR, addr);
	reg &= ~R92C_EFUSE_CTRL_VALID;
	rtwn_m1_write_4(sc, R92C_EFUSE_CTRL, reg);

	/* Tunggu operasi baca selesai. */
	for (ntries = 0; ntries < 100; ntries++) {
		reg = rtwn_m1_read_4(sc, R92C_EFUSE_CTRL);
		if (reg & R92C_EFUSE_CTRL_VALID) {
			return (uint8_t)MS(reg, R92C_EFUSE_CTRL_DATA);
		}
		RTWN_DELAY(5);
	}

	IOLog(LOGP "efuse: gagal baca byte pada alamat 0x%04x\n", addr);
	return 0xff;
}

/* OpenBSD: rtwn_efuse_switch_power() */
static void
rtwn_m1_efuse_switch_power(rtwn_m1_t *sc)
{
	uint16_t reg;

	if (!(sc->chip & (RTWN_CHIP_88F | RTWN_CHIP_92E))) {
		reg = rtwn_m1_read_2(sc, R92C_SYS_ISO_CTRL);
		if (!(reg & R92C_SYS_ISO_CTRL_PWC_EV12V)) {
			rtwn_m1_write_2(sc, R92C_SYS_ISO_CTRL,
			    reg | R92C_SYS_ISO_CTRL_PWC_EV12V);
		}
	}
	reg = rtwn_m1_read_2(sc, R92C_SYS_FUNC_EN);
	if (!(reg & R92C_SYS_FUNC_EN_ELDR)) {
		rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
		    reg | R92C_SYS_FUNC_EN_ELDR);
	}
	reg = rtwn_m1_read_2(sc, R92C_SYS_CLKR);
	if ((reg & (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) !=
	    (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) {
		rtwn_m1_write_2(sc, R92C_SYS_CLKR,
		    reg | R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M);
	}
}

/* OpenBSD: rtwn_efuse_read() */
static void
rtwn_m1_efuse_read(rtwn_m1_t *sc, uint8_t *rom, size_t size)
{
	uint8_t off, msk, tmp;
	uint16_t addr = 0;
	uint32_t reg;
	int i, len;

	if (!(sc->chip & (RTWN_CHIP_92C | RTWN_CHIP_88C))) {
		rtwn_m1_write_1(sc, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_ON);
	}

	rtwn_m1_efuse_switch_power(sc);

	memset(rom, 0xff, size);
	len = (sc->chip & RTWN_CHIP_88E) ? 256 : 512;

	while (addr < len) {
		reg = rtwn_m1_efuse_read_1(sc, addr);
		if (reg == 0xff) {
			break;
		}
		addr++;

		/* Header ekstensi? */
		if ((sc->flags & RTWN_FLAG_EXT_HDR) && (reg & 0x1f) == 0x0f) {
			tmp = (reg & 0xe0) >> 5;
			reg = rtwn_m1_efuse_read_1(sc, addr);
			addr++;
			if ((reg & 0x0f) != 0x0f) {
				off = (uint8_t)(((reg & 0xf0) >> 1) | tmp);
			} else {
				continue;
			}
		} else {
			off = (uint8_t)(reg >> 4);
		}
		msk = (uint8_t)(reg & 0xf);
		for (i = 0; i < 4; i++) {
			if (msk & (1 << i)) {
				continue;
			}
			if ((size_t)(off * 8 + i * 2 + 0) < size) {
				rom[off * 8 + i * 2 + 0] =
				    rtwn_m1_efuse_read_1(sc, addr);
			}
			addr++;
			if ((size_t)(off * 8 + i * 2 + 1) < size) {
				rom[off * 8 + i * 2 + 1] =
				    rtwn_m1_efuse_read_1(sc, addr);
			}
			addr++;
		}
	}

	if (!(sc->chip & (RTWN_CHIP_92C | RTWN_CHIP_88C))) {
		rtwn_m1_write_1(sc, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_OFF);
	}
}

/* ------------------------------------------------------------------ */
/* Chip ID & ROM (OpenBSD: rtwn.c rtwn_read_chipid / rtwn_r88e_read_rom) */
/* ------------------------------------------------------------------ */

int
rtwn_m1_read_chipid(rtwn_m1_t *sc)
{
	uint32_t reg;

	if (sc->chip & (RTWN_CHIP_88E | RTWN_CHIP_88F | RTWN_CHIP_92E)) {
		/* 88E/88F/92E: ID dibaca dari header firmware/ROM, bukan register. */
		sc->flags |= RTWN_FLAG_EXT_HDR;
		IOLog(LOGP "chip: 88E/88F/92E detected (extended header)\n");
		return RTWN_M1_OK;
	}

	reg = rtwn_m1_read_4(sc, R92C_SYS_CFG);
	if (reg & R92C_SYS_CFG_TRP_VAUX_EN) {
		IOLog(LOGP "chip: test chip (TRP_VAUX_EN) — unsupported\n");
		return RTWN_M1_ERR_CHIP;
	}

	if (sc->chip & (RTWN_CHIP_92C | RTWN_CHIP_88C)) {
		if (reg & R92C_SYS_CFG_TYPE_92C) {
			sc->chip &= ~RTWN_CHIP_88C;
			if (MS(rtwn_m1_read_4(sc, R92C_HPON_FSM),
			    R92C_HPON_FSM_CHIP_BONDING_ID) ==
			    R92C_HPON_FSM_CHIP_BONDING_ID_92C_1T2R) {
				sc->chip |= RTWN_CHIP_92C_1T2R;
			}
		} else {
			sc->chip &= ~RTWN_CHIP_92C;
		}
		if (reg & R92C_SYS_CFG_VENDOR_UMC) {
			sc->chip |= RTWN_CHIP_UMC;
			if (MS(reg, R92C_SYS_CFG_CHIP_VER_RTL) == 0) {
				sc->chip |= RTWN_CHIP_UMC_A_CUT;
			}
		}
		return RTWN_M1_OK;
	} else if (sc->chip & RTWN_CHIP_23A) {
		sc->flags |= RTWN_FLAG_EXT_HDR;
		if ((reg & 0xf000) == 0) {
			sc->chip |= RTWN_CHIP_UMC_A_CUT;
		}
		return RTWN_M1_OK;
	}

	IOLog(LOGP "chip: unsupported (chip flags 0x%08x)\n", sc->chip);
	return RTWN_M1_ERR_CHIP;
}

/* Offset MAC address di dalam image ROM 88E (USB variant).
 * struct r88e_rom → union u.r88eu → macaddr  (offset 215 dari awal image).
 * Dihitung dengan offsetof supaya tidak salah tulis. */
#define R88E_ROM_MAC_OFFSET offsetof(struct r88e_rom, u.r88eu.macaddr)

int
rtwn_m1_read_rom(rtwn_m1_t *sc)
{
	struct r88e_rom *rom = (struct r88e_rom *)sc->rom;

	rtwn_m1_efuse_read(sc, sc->rom, sizeof(sc->rom));

	/* USB 88E: crystal cap = rom->xtal (bukan 0x20 seperti PCIe). */
	sc->xtal = rom->xtal;
	sc->regulatory = (uint8_t)MS(rom->rf_board_opt, R92C_ROM_RF1_REGULATORY);
	sc->channel_plan = rom->channel_plan;

	memcpy(sc->macaddr, sc->rom + R88E_ROM_MAC_OFFSET, 6);
	sc->efuseValid = true;

	IOLog(LOGP "rom: xtal=0x%02x regulatory=%u channel_plan=%u\n",
	    sc->xtal, sc->regulatory, sc->channel_plan);
	IOLog(LOGP "rom: MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
	    sc->macaddr[0], sc->macaddr[1], sc->macaddr[2],
	    sc->macaddr[3], sc->macaddr[4], sc->macaddr[5]);
	return RTWN_M1_OK;
}

/* ------------------------------------------------------------------ */
/* Power on (OpenBSD: if_urtwn.c urtwn_r88e_power_on)                  */
/* ------------------------------------------------------------------ */

int
rtwn_m1_power_on(rtwn_m1_t *sc)
{
	uint32_t reg;
	int ntries;

	/* Tunggu bit power ready. */
	for (ntries = 0; ntries < 5000; ntries++) {
		if (rtwn_m1_read_4(sc, R92C_APS_FSMCO) & R92C_APS_FSMCO_SUS_HOST) {
			break;
		}
		RTWN_DELAY(10);
	}
	if (ntries == 5000) {
		IOLog(LOGP "power on: timeout menunggu chip power up\n");
		return RTWN_M1_ERR_POWERON;
	}

	/* Reset BB. */
	rtwn_m1_write_1(sc, R92C_SYS_FUNC_EN,
	    rtwn_m1_read_1(sc, R92C_SYS_FUNC_EN) &
	    ~(R92C_SYS_FUNC_EN_BBRSTB | R92C_SYS_FUNC_EN_BB_GLB_RST));

	rtwn_m1_write_1(sc, R92C_AFE_XTAL_CTRL + 2,
	    rtwn_m1_read_1(sc, R92C_AFE_XTAL_CTRL + 2) | 0x80);

	/* Matikan HWPDN. */
	rtwn_m1_write_2(sc, R92C_APS_FSMCO,
	    rtwn_m1_read_2(sc, R92C_APS_FSMCO) & ~R92C_APS_FSMCO_APDM_HPDN);
	/* Matikan WL suspend. */
	rtwn_m1_write_2(sc, R92C_APS_FSMCO,
	    rtwn_m1_read_2(sc, R92C_APS_FSMCO) &
	    ~(R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_AFSM_PCIE));

	/* Auto enable WLAN. */
	rtwn_m1_write_2(sc, R92C_APS_FSMCO,
	    rtwn_m1_read_2(sc, R92C_APS_FSMCO) | R92C_APS_FSMCO_APFM_ONMAC);
	for (ntries = 0; ntries < 5000; ntries++) {
		if (!(rtwn_m1_read_2(sc, R92C_APS_FSMCO) &
		    R92C_APS_FSMCO_APFM_ONMAC)) {
			break;
		}
		RTWN_DELAY(10);
	}
	if (ntries == 5000) {
		IOLog(LOGP "power on: timeout menunggu MAC auto ON\n");
		return RTWN_M1_ERR_POWERON;
	}

	/* LDO normal mode. */
	rtwn_m1_write_1(sc, R92C_LPLDO_CTRL,
	    rtwn_m1_read_1(sc, R92C_LPLDO_CTRL) & ~0x10);

	/* Aktifkan blok MAC DMA/WMAC/SCHEDULE/SEC. */
	rtwn_m1_write_2(sc, R92C_CR, 0);
	reg = rtwn_m1_read_2(sc, R92C_CR);
	reg |= R92C_CR_HCI_TXDMA_EN | R92C_CR_HCI_RXDMA_EN |
	    R92C_CR_TXDMA_EN | R92C_CR_RXDMA_EN | R92C_CR_PROTOCOL_EN |
	    R92C_CR_SCHEDULE_EN | R92C_CR_ENSEC | R92C_CR_CALTMR_EN;
	rtwn_m1_write_2(sc, R92C_CR, reg);

	IOLog(LOGP "power on: OK\n");
	return RTWN_M1_OK;
}

/* ------------------------------------------------------------------ */
/* DMA init (OpenBSD: if_urtwn.c urtwn_dma_init + urtwn_llt_init)      */
/* ------------------------------------------------------------------ */

/* OpenBSD: urtwn_llt_write() */
static int
rtwn_m1_llt_write(rtwn_m1_t *sc, uint32_t addr, uint32_t data)
{
	int ntries;

	rtwn_m1_write_4(sc, R92C_LLT_INIT,
	    SM(R92C_LLT_INIT_OP, R92C_LLT_INIT_OP_WRITE) |
	    SM(R92C_LLT_INIT_ADDR, addr) |
	    SM(R92C_LLT_INIT_DATA, data));
	/* Tunggu operasi tulis selesai. */
	for (ntries = 0; ntries < 20; ntries++) {
		if (MS(rtwn_m1_read_4(sc, R92C_LLT_INIT), R92C_LLT_INIT_OP) ==
		    R92C_LLT_INIT_OP_NO_ACTIVE) {
			return 0;
		}
		RTWN_DELAY(5);
	}
	IOLog(LOGP "llt init: timeout saat menulis LLT\n");
	return RTWN_M1_ERR_POWERON;
}

/* OpenBSD: urtwn_llt_init() */
static int
rtwn_m1_llt_init(rtwn_m1_t *sc, int page_count)
{
	int i, error, pktbuf_count;

	pktbuf_count = (sc->chip & RTWN_CHIP_88E) ?
	    R88E_TXPKTBUF_COUNT : R92C_TXPKTBUF_COUNT;

	/* Reserve pages [0; page_count]. */
	for (i = 0; i < page_count; i++) {
		if ((error = rtwn_m1_llt_write(sc, i, i + 1)) != 0) {
			return error;
		}
	}
	/* 0xff = akhir list. */
	if ((error = rtwn_m1_llt_write(sc, i, 0xff)) != 0) {
		return error;
	}
	/* Pages [page_count + 1; pktbuf_count - 1] sebagai ring buffer. */
	for (++i; i < pktbuf_count - 1; i++) {
		if ((error = rtwn_m1_llt_write(sc, i, i + 1)) != 0) {
			return error;
		}
	}
	/* Page terakhir menunjuk ke awal ring buffer. */
	return rtwn_m1_llt_write(sc, i, page_count + 1);
}

int
rtwn_m1_dma_init(rtwn_m1_t *sc)
{
	uint32_t reg;
	uint16_t dmasize;
	int hqpages, lqpages, nqpages, pagecnt, boundary;
	int error, hashq, haslq, hasnq;

	/* Nilai default per chipset. */
	if (sc->chip & RTWN_CHIP_88E) {
		hqpages = R88E_HQ_NPAGES;
		lqpages = R88E_LQ_NPAGES;
		nqpages = R88E_NQ_NPAGES;
		pagecnt = R88E_TX_PAGE_COUNT;
		dmasize = R88E_MAX_RX_DMA_SIZE;
	} else if (sc->chip & RTWN_CHIP_88F) {
		hqpages = R88F_HQ_NPAGES;
		lqpages = R88F_LQ_NPAGES;
		nqpages = R88F_NQ_NPAGES;
		pagecnt = R88F_TX_PAGE_COUNT;
		dmasize = R88F_MAX_RX_DMA_SIZE;
	} else if (sc->chip & RTWN_CHIP_92E) {
		hqpages = R92E_HQ_NPAGES;
		lqpages = R92E_LQ_NPAGES;
		nqpages = R92E_NQ_NPAGES;
		pagecnt = R92E_TX_PAGE_COUNT;
		dmasize = R92E_MAX_RX_DMA_SIZE;
	} else {
		hqpages = R92C_HQ_NPAGES;
		lqpages = R92C_LQ_NPAGES;
		nqpages = R92C_NQ_NPAGES;
		pagecnt = R92C_TX_PAGE_COUNT;
		dmasize = R92C_MAX_RX_DMA_SIZE;
	}
	boundary = pagecnt + 1;

	/* Inisialisasi tabel LLT. */
	if (sc->chip & (RTWN_CHIP_88F | RTWN_CHIP_92E)) {
		/* Auto LLT tidak diimplementasikan pada M1 (khchip 88E). */
		IOLog(LOGP "dma init: auto LLT belum diimplementasikan\n");
		return RTWN_M1_ERR_POWERON;
	}
	if ((error = rtwn_m1_llt_init(sc, pagecnt)) != 0) {
		return error;
	}

	/* Pemetaan antrean Tx ke endpoint USB. */
	hashq = hasnq = haslq = 0;
	switch (sc->ntx) {
	case 3:
		haslq = 1;
		pagecnt -= lqpages;
		/* FALLTHROUGH */
	case 2:
		hasnq = 1;
		pagecnt -= nqpages;
		/* FALLTHROUGH */
	case 1:
		hashq = 1;
		pagecnt -= hqpages;
		break;
	}

	/* Jumlah page untuk antrean normal priority. */
	rtwn_m1_write_1(sc, R92C_RQPN_NPQ, hasnq ? nqpages : 0);
	rtwn_m1_write_4(sc, R92C_RQPN,
	    SM(R92C_RQPN_PUBQ, pagecnt) |
	    SM(R92C_RQPN_HPQ, hashq ? hqpages : 0) |
	    SM(R92C_RQPN_LPQ, haslq ? lqpages : 0) |
	    R92C_RQPN_LD);

	rtwn_m1_write_1(sc, R92C_TXPKTBUF_BCNQ_BDNY, boundary);
	rtwn_m1_write_1(sc, R92C_TXPKTBUF_MGQ_BDNY, boundary);
	rtwn_m1_write_1(sc, R92C_TXPKTBUF_WMAC_LBK_BF_HD, boundary);
	rtwn_m1_write_1(sc, R92C_TRXFF_BNDY, boundary);
	rtwn_m1_write_1(sc, R92C_TDECTRL + 1, boundary);

	/* Pemetaan antrean ke USB pipe. */
	reg = rtwn_m1_read_2(sc, R92C_TRXDMA_CTRL);
	reg &= ~R92C_TRXDMA_CTRL_QMAP_M;
	if (haslq) {
		reg |= R92C_TRXDMA_CTRL_QMAP_3EP;
	} else if (hashq) {
		if (!hasnq) {
			reg |= R92C_TRXDMA_CTRL_QMAP_HQ;
		} else {
			reg |= R92C_TRXDMA_CTRL_QMAP_HQ_NQ;
		}
	}
	rtwn_m1_write_2(sc, R92C_TRXDMA_CTRL, reg);

	/* Batas halaman transfer Tx/Rx. */
	rtwn_m1_write_2(sc, R92C_TRXFF_BNDY + 2, dmasize - 1);

	/* Ukuran halaman transfer Tx/Rx. */
	if (!(sc->chip & RTWN_CHIP_92E)) {
		if (sc->chip & RTWN_CHIP_88F) {
			rtwn_m1_write_1(sc, R92C_PBP,
			    SM(R92C_PBP_PSRX, R92C_PBP_256) |
			    SM(R92C_PBP_PSTX, R92C_PBP_256));
		} else {
			rtwn_m1_write_1(sc, R92C_PBP,
			    SM(R92C_PBP_PSRX, R92C_PBP_128) |
			    SM(R92C_PBP_PSTX, R92C_PBP_128));
		}
	}

	sc->dmaReady = true;
	IOLog(LOGP "dma init: OK (ntx=%d, pagecnt=%d)\n", sc->ntx, pagecnt);
	return RTWN_M1_OK;
}

/* ------------------------------------------------------------------ */
/* Firmware (OpenBSD: if_urtwn.c urtwn_fw_loadpage,                    */
/*              rtwn.c rtwn_load_firmware + rtwn_r88e_fw_reset)         */
/* ------------------------------------------------------------------ */

/* OpenBSD: rtwn_r88e_fw_reset() */
static void
rtwn_m1_fw_reset(rtwn_m1_t *sc)
{
	if (!(sc->chip & RTWN_CHIP_88F)) {
		rtwn_m1_write_1(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_1(sc, R92C_RSV_CTRL) & ~R92C_RSV_CTRL_WLOCK_00);
	}
	if (sc->chip & RTWN_CHIP_88E) {
		rtwn_m1_write_2(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_2(sc, R92C_RSV_CTRL) & ~R88E_RSV_CTRL_MCU_RST);
	} else {
		rtwn_m1_write_2(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_2(sc, R92C_RSV_CTRL) & ~R88E_RSV_CTRL_MIO_EN);
	}
	rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
	    rtwn_m1_read_2(sc, R92C_SYS_FUNC_EN) & ~R92C_SYS_FUNC_EN_CPUEN);

	/* Enable MCU IO wrapper. */
	if (!(sc->chip & RTWN_CHIP_88F)) {
		rtwn_m1_write_1(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_1(sc, R92C_RSV_CTRL) & ~R92C_RSV_CTRL_WLOCK_00);
	}
	if (sc->chip & RTWN_CHIP_88E) {
		rtwn_m1_write_2(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_2(sc, R92C_RSV_CTRL) | R88E_RSV_CTRL_MCU_RST);
	} else {
		rtwn_m1_write_2(sc, R92C_RSV_CTRL,
		    rtwn_m1_read_2(sc, R92C_RSV_CTRL) | R88E_RSV_CTRL_MIO_EN);
	}
	rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
	    rtwn_m1_read_2(sc, R92C_SYS_FUNC_EN) | R92C_SYS_FUNC_EN_CPUEN);
}

/* OpenBSD: urtwn_fw_loadpage() */
static int
rtwn_m1_fw_loadpage(rtwn_m1_t *sc, int page, const uint8_t *buf, int len)
{
	uint32_t reg;
	int maxblksz, off, mlen, error = 0;

	reg = rtwn_m1_read_4(sc, R92C_MCUFWDL);
	reg = RW(reg, R92C_MCUFWDL_PAGE, page);
	rtwn_m1_write_4(sc, R92C_MCUFWDL, reg);

	maxblksz = (sc->chip & RTWN_CHIP_92E) ? 254 : 196;

	off = R92C_FW_START_ADDR;
	while (len > 0) {
		if (len > maxblksz) {
			mlen = maxblksz;
		} else if (len > 4) {
			mlen = 4;
		} else {
			mlen = 1;
		}
		error = rtwn_m1_write_region(sc, (uint16_t)off, buf, mlen);
		if (error != 0) {
			break;
		}
		off += mlen;
		buf += mlen;
		len -= mlen;
	}
	return error;
}

/* OpenBSD: rtwn_load_firmware() */
int
rtwn_m1_load_firmware(rtwn_m1_t *sc)
{
	const struct r92c_fw_hdr *hdr;
	const uint8_t *ptr;
	size_t len;
	uint32_t reg;
	int mlen, ntries, page, error;

	ptr = rtl8188eufw_bin;
	len = rtl8188eufw_bin_len;

	if (len < sizeof(*hdr)) {
		IOLog(LOGP "firmware: terlalu pendek\n");
		return RTWN_M1_ERR_FIRMWARE;
	}

	hdr = (const struct r92c_fw_hdr *)ptr;
	/* Lewati header firmware bila signature cocok. */
	if ((hdr->signature >> 4) == 0x230 || (hdr->signature >> 4) == 0x88c ||
	    (hdr->signature >> 4) == 0x88e || (hdr->signature >> 4) == 0x88f ||
	    (hdr->signature >> 4) == 0x92c || (hdr->signature >> 4) == 0x92e) {
		IOLog(LOGP "firmware: signature=0x%04x v%d.%d (%02d-%02d %02d:%02d), "
		    "header %u byte dilewati\n",
		    hdr->signature, hdr->version, hdr->subversion,
		    hdr->month, hdr->date, hdr->hour, hdr->minute,
		    (unsigned)sizeof(*hdr));
		ptr += sizeof(*hdr);
		len -= sizeof(*hdr);
	} else {
		IOLog(LOGP "firmware: signature tidak dikenali (0x%04x) — "
		    "coba upload apa adanya\n", hdr->signature);
	}

	if (rtwn_m1_read_1(sc, R92C_MCUFWDL) & R92C_MCUFWDL_RAM_DL_SEL) {
		rtwn_m1_write_1(sc, R92C_MCUFWDL, 0);
		rtwn_m1_fw_reset(sc);
	}

	if ((sc->chip & RTWN_CHIP_PCI) || (sc->chip & RTWN_CHIP_88F)) {
		rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
		    rtwn_m1_read_2(sc, R92C_SYS_FUNC_EN) | R92C_SYS_FUNC_EN_CPUEN);
	}

	/* Aktifkan mode download firmware. */
	rtwn_m1_write_1(sc, R92C_MCUFWDL,
	    rtwn_m1_read_1(sc, R92C_MCUFWDL) | R92C_MCUFWDL_EN);
	rtwn_m1_write_4(sc, R92C_MCUFWDL,
	    rtwn_m1_read_4(sc, R92C_MCUFWDL) & ~R92C_MCUFWDL_ROM_DLEN);

	/* Reset checksum FWDL. */
	rtwn_m1_write_1(sc, R92C_MCUFWDL,
	    rtwn_m1_read_1(sc, R92C_MCUFWDL) | R92C_MCUFWDL_CHKSUM_RPT);

	RTWN_DELAY(50);

	for (page = 0; len > 0; page++) {
		mlen = (len > R92C_FW_PAGE_SIZE) ? R92C_FW_PAGE_SIZE : (int)len;
		error = rtwn_m1_fw_loadpage(sc, page, ptr, mlen);
		if (error != 0) {
			IOLog(LOGP "firmware: gagal load page %d\n", page);
			return RTWN_M1_ERR_FIRMWARE;
		}
		ptr += mlen;
		len -= mlen;
	}

	/* Tunggu laporan checksum. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (rtwn_m1_read_4(sc, R92C_MCUFWDL) & R92C_MCUFWDL_CHKSUM_RPT) {
			break;
		}
		RTWN_DELAY(5);
	}
	if (ntries == 1000) {
		IOLog(LOGP "firmware: timeout menunggu checksum report\n");
		return RTWN_M1_ERR_FIRMWARE;
	}
	IOLog(LOGP "firmware: checksum report OK\n");

	/* Nonaktifkan mode download. */
	rtwn_m1_write_1(sc, R92C_MCUFWDL,
	    rtwn_m1_read_1(sc, R92C_MCUFWDL) & ~R92C_MCUFWDL_EN);

	/* Reserved untuk ekstensi firmware. */
	if (!(sc->chip & (RTWN_CHIP_88F | RTWN_CHIP_92E))) {
		rtwn_m1_write_1(sc, R92C_MCUFWDL + 1, 0);
	}

	reg = rtwn_m1_read_4(sc, R92C_MCUFWDL);
	reg = (reg & ~R92C_MCUFWDL_WINTINI_RDY) | R92C_MCUFWDL_RDY;
	rtwn_m1_write_4(sc, R92C_MCUFWDL, reg);

	if (sc->chip & (RTWN_CHIP_92C | RTWN_CHIP_88C | RTWN_CHIP_23A)) {
		reg = rtwn_m1_read_2(sc, R92C_SYS_FUNC_EN);
		rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
		    reg & ~R92C_SYS_FUNC_EN_CPUEN);
		rtwn_m1_write_2(sc, R92C_SYS_FUNC_EN,
		    reg | R92C_SYS_FUNC_EN_CPUEN);
	} else {
		rtwn_m1_fw_reset(sc);
	}

	/* Tunggu firmware siap. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (rtwn_m1_read_4(sc, R92C_MCUFWDL) & R92C_MCUFWDL_WINTINI_RDY) {
			break;
		}
		RTWN_DELAY(10);
	}
	if (ntries == 1000) {
		IOLog(LOGP "firmware: timeout menunggu firmware readiness\n");
		return RTWN_M1_ERR_FIRMWARE;
	}

	sc->fwLoaded = true;
	IOLog(LOGP "firmware: upload OK (%u byte)\n", rtl8188eufw_bin_len);
	return RTWN_M1_OK;
}
