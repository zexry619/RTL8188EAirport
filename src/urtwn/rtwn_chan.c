/*
 * rtwn_chan.c — pengaturan kanal & akses register RF/BB untuk 8188E.
 *
 * Port dari OpenBSD rtwn.c (lisensi ISC):
 *   rtwn_set_chan()        rtwn.c:2408
 *   rtwn_rf_write()        rtwn.c:438
 *   rtwn_rf_read()         rtwn.c:463
 *
 * Diporting ke lapisan portabel kita (src/urtwn) memakai callback
 * read/write yang sudah ada (rtwn_seq_raw_read1/write1/read4/write4).
 *
 * CATATAN PENTING untuk 8188E (USB, 1 chain):
 *   - rtwn_bb_write = rtwn_write_4  (tulis 32-bit!)
 *   - rtwn_bb_read  = rtwn_read_4
 *   - param_addr pakai R88E_LSSI_PARAM_ADDR (bit 20..27), bukan
 *     R92C_LSSI_PARAM_ADDR (bit 20..25)
 *   - pada rtwn_rf_read, blok "restore READ_EDGE" DIHILANGKAN untuk 88E
 *     (hanya dilakukan untuk chip non-88E)
 */

#include "rtwn_chan.h"

#include <string.h>

/* ── register (dari OpenBSD r92creg.h) ─────────────────────────────── */
#define R92C_BWOPMODE			0x603
#define R92C_BWOPMODE_20MHZ		0x04

#define R92C_FPGA0_RFMOD		0x800
#define R92C_FPGA1_RFMOD		0x900
#define R92C_RFMOD_40MHZ		0x00000001

#define R92C_HSSI_PARAM1(chain)		(0x820 + (chain) * 8)
#define R92C_HSSI_PARAM2(chain)		(0x824 + (chain) * 8)
#define R92C_HSSI_PARAM1_PI		0x00000100
#define R92C_HSSI_PARAM2_READ_ADDR_M	0x7f800000
#define R92C_HSSI_PARAM2_READ_ADDR_S	23
#define R92C_HSSI_PARAM2_READ_EDGE	0x80000000

#define R92C_LSSI_PARAM(chain)		(0x840 + (chain) * 4)
#define R92C_LSSI_PARAM_DATA_M		0x000fffff
#define R92C_LSSI_PARAM_DATA_S		0
#define R88E_LSSI_PARAM_ADDR_M		0x0ff00000
#define R88E_LSSI_PARAM_ADDR_S		20

#define R92C_LSSI_READBACK(chain)	(0x8a0 + (chain) * 4)
#define R92C_HSPI_READBACK(chain)	(0x8b8 + (chain) * 4)
#define R92C_LSSI_READBACK_DATA_M	0x000fffff
#define R92C_LSSI_READBACK_DATA_S	0

#define R92C_RF_CHNLBW			0x18
#define R88E_RF_CHNLBW_BW40		0x00400
#define R88E_RF_CHNLBW_BW20		0x00c00

#define R92C_RRSR_HI			0x442
#define R92C_CCK0_SYSTEM		0xa00
#define R92C_OFDM0_AGCCORE1_0		0xc50
#define R92C_OFDM1_LSTF			0xd00

/* BSD-style SM(field, val) = ((val << S) & M) */
#define RTWN_SM(mask, shift, val)	(((uint32_t)(val) << (shift)) & (mask))

/* ── helper baca/tulis 32-bit (bb) ─────────────────────────────────── */

static uint32_t
rtwn_bb_read(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t reg)
{
	uint32_t v = 0;

	if (rd(ctx, reg, &v, 4) != 0) {
		return 0xffffffffu;
	}
	return v;
}

static int
rtwn_bb_write(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t reg,
    uint32_t val)
{
	return wr(ctx, reg, &val, 4);
}

/*
 * Baca register RF (jalur readback 8188E).
 *
 * Mirip rtwn_rf_read() OpenBSD, minus blok restore READ_EDGE yang
 * hanya dipakai chip non-88E.
 */
uint32_t
rtwn_rf_read_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int chain,
    uint8_t addr)
{
	uint32_t reg0, regc, val;

	if (chain < 0 || chain > 1) {
		return 0;
	}

	reg0 = rtwn_bb_read(rd, wr, ctx, (uint16_t)R92C_HSSI_PARAM2(0));
	regc = (chain != 0) ?
	    rtwn_bb_read(rd, wr, ctx, (uint16_t)R92C_HSSI_PARAM2(chain)) : reg0;

	/* matikan READ_EDGE di chain 0 */
	rtwn_bb_write(rd, wr, ctx, (uint16_t)R92C_HSSI_PARAM2(0),
	    reg0 & ~((uint32_t)R92C_HSSI_PARAM2_READ_EDGE));
	RTWN_DELAY_MS(1);

	/* set alamat baca + picu READ_EDGE */
	{
		uint32_t v = (regc & ~((uint32_t)R92C_HSSI_PARAM2_READ_ADDR_M)) |
		    RTWN_SM(R92C_HSSI_PARAM2_READ_ADDR_M,
			R92C_HSSI_PARAM2_READ_ADDR_S, addr) |
		    (uint32_t)R92C_HSSI_PARAM2_READ_EDGE;
		rtwn_bb_write(rd, wr, ctx,
		    (uint16_t)R92C_HSSI_PARAM2(chain), v);
	}
	RTWN_DELAY_MS(1);

	/* baca back */
	if (rtwn_bb_read(rd, wr, ctx,
	    (uint16_t)R92C_HSSI_PARAM1(chain)) & (uint32_t)R92C_HSSI_PARAM1_PI) {
		val = rtwn_bb_read(rd, wr, ctx,
		    (uint16_t)R92C_HSPI_READBACK(chain));
	} else {
		val = rtwn_bb_read(rd, wr, ctx,
		    (uint16_t)R92C_LSSI_READBACK(chain));
	}
	return (val >> R92C_LSSI_READBACK_DATA_S) & R92C_LSSI_READBACK_DATA_M;
}

/*
 * Tulis register RF (jalur LSSI parameter 8188E).
 */
int
rtwn_rf_write_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, int chain,
    uint16_t addr, uint32_t val)
{
	uint32_t param_addr;

	if (chain < 0 || chain > 1) {
		return -1;
	}

	/* 8188E pakai field addr 8 bit di bit 20..27 */
	param_addr = RTWN_SM(R88E_LSSI_PARAM_ADDR_M, R88E_LSSI_PARAM_ADDR_S,
	    addr & 0xff);

	rtwn_bb_write(rd, wr, ctx, (uint16_t)R92C_LSSI_PARAM(chain),
	    param_addr | RTWN_SM(R92C_LSSI_PARAM_DATA_M,
		R92C_LSSI_PARAM_DATA_S, val & 0xfffff));

	RTWN_DELAY_MS(1);
	return 0;
}

/*
 * Set gain awal AGC RX pada chain 0:
 *   - 0x20 saat scanning (sensitivitas maksimum)
 *   - 0x32 saat terhubung (mencegah saturasi LNA / interferensi dekat)
 * Sesuai OpenBSD rtwn_newstate() (rtwn.c:1118, 1144).
 */
void
rtwn_set_rx_gain_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t gain)
{
	uint32_t v = rtwn_bb_read(rd, wr, ctx, R92C_OFDM0_AGCCORE1_0);
	if (v == 0xffffffffu) {
		return;
	}
	v = (v & ~0x0000007fu) | ((uint32_t)gain & 0x7fu);
	rtwn_bb_write(rd, wr, ctx, R92C_OFDM0_AGCCORE1_0, v);
}

/*
 * Set kanal & bandwidth (20 MHz / 40 MHz HT40+ / HT40-) untuk 8188E 1x1 2,4 GHz.
 *
 * Diverifikasi silang antara:
 *   - RtWlanU.kext: phy_SetBWMode8188E (@0x146469),
 *     PHY_RF6052SetBandwidth8188E (@0x1475b0), dan
 *     CHNL_GetCenterFrequency (@0xbd050)
 *   - OpenBSD rtwn_set_chan() (rtwn.c:2408)
 *
 * @chan    : nomor kanal primer 1..14
 * @bw40    : 1 untuk 40 MHz (HT40), 0 untuk 20 MHz
 * @prichlo : 1 bila kanal primer di bawah sekunder (HT40+ / SCA -> center = chan + 2),
 *            0 bila kanal primer di atas sekunder (HT40- / SCB -> center = chan - 2)
 */
int
rtwn_set_chan_bw_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t chan, int bw40, int prichlo)
{
	uint32_t v;
	uint8_t v8;

	if (chan < 1 || chan > 14) {
		return -1;
	}

	if (bw40) {
		uint8_t center = prichlo ? (uint8_t)(chan + 2) :
		    (chan > 2 ? (uint8_t)(chan - 2) : 1);
		if (center < 1)
			center = 1;
		if (center > 14)
			center = 14;

		/* 1. Mode bandwidth 40 MHz di MAC (BWOPMODE &= ~20MHZ, RRSR+2 subchannel) */
		v8 = rtwn_seq_raw_read1(rd, wr, ctx, R92C_BWOPMODE);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_BWOPMODE,
		    (uint8_t)(v8 & ~R92C_BWOPMODE_20MHZ));

		v8 = rtwn_seq_raw_read1(rd, wr, ctx, R92C_RRSR_HI);
		v8 = (uint8_t)((v8 & ~0x6f) | ((prichlo ? 1 : 2) << 5));
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_RRSR_HI, v8);

		/* 2. Aktifkan 40 MHz & pilih sideband CCK/OFDM di BB */
		v = rtwn_bb_read(rd, wr, ctx, R92C_FPGA0_RFMOD);
		rtwn_bb_write(rd, wr, ctx, R92C_FPGA0_RFMOD,
		    v | (uint32_t)R92C_RFMOD_40MHZ);

		v = rtwn_bb_read(rd, wr, ctx, R92C_FPGA1_RFMOD);
		rtwn_bb_write(rd, wr, ctx, R92C_FPGA1_RFMOD,
		    v | (uint32_t)R92C_RFMOD_40MHZ);

		v = rtwn_bb_read(rd, wr, ctx, R92C_CCK0_SYSTEM);
		v = (v & ~0x00000010u) | ((uint32_t)(prichlo ? 0 : 1) << 4);
		rtwn_bb_write(rd, wr, ctx, R92C_CCK0_SYSTEM, v);

		v = rtwn_bb_read(rd, wr, ctx, R92C_OFDM1_LSTF);
		v = (v & ~0x00000c00u) | ((uint32_t)(prichlo ? 1 : 2) << 10);
		rtwn_bb_write(rd, wr, ctx, R92C_OFDM1_LSTF, v);

		v = rtwn_bb_read(rd, wr, ctx, 0x818);
		v = (v & ~0x0c000000u) | ((uint32_t)(prichlo ? 2 : 1) << 26);
		rtwn_bb_write(rd, wr, ctx, 0x818, v);

		/* 3. RF CHNLBW (chain 0): center channel + BW40 (0x400 pada 8188E!) */
		v = rtwn_rf_read_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW);
		v = (v & ~0xfffu) | (uint32_t)center |
		    (uint32_t)R88E_RF_CHNLBW_BW40;
		rtwn_rf_write_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW, v);
	} else {
		/* 1. mode bandwidth 20 MHz */
		v8 = rtwn_seq_raw_read1(rd, wr, ctx, R92C_BWOPMODE);
		rtwn_seq_raw_write1(rd, wr, ctx, R92C_BWOPMODE,
		    (uint8_t)(v8 | R92C_BWOPMODE_20MHZ));

		/* 2. matikan flag 40 MHz di FPGA RFMOD */
		v = rtwn_bb_read(rd, wr, ctx, R92C_FPGA0_RFMOD);
		rtwn_bb_write(rd, wr, ctx, R92C_FPGA0_RFMOD,
		    v & ~((uint32_t)R92C_RFMOD_40MHZ));

		v = rtwn_bb_read(rd, wr, ctx, R92C_FPGA1_RFMOD);
		rtwn_bb_write(rd, wr, ctx, R92C_FPGA1_RFMOD,
		    v & ~((uint32_t)R92C_RFMOD_40MHZ));

		/* 3. RF CHNLBW — chain 0 saja (8188E 1x1) */
		v = rtwn_rf_read_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW);
		v = (v & ~0xfffu) | (uint32_t)chan |
		    (uint32_t)R88E_RF_CHNLBW_BW20;
		rtwn_rf_write_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW, v);
	}

	return 0;
}

int
rtwn_set_chan_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint8_t chan)
{
	return rtwn_set_chan_bw_8188e(rd, wr, ctx, chan, 0, 0);
}

/* ── TX Power dari EFUSE (rtwn_r88e_get_txpower + rtwn_write_txpower) ── */

static int8_t
sign4to8(uint8_t v)
{
	v &= 0x0f;
	return (v & 0x08) ? (int8_t)(v | 0xf0) : (int8_t)v;
}

static uint8_t
clamp_txpwr(int v)
{
	if (v < 0)
		return 0;
	if (v > 0x3f)
		return 0x3f;
	return (uint8_t)v;
}

void
rtwn_set_txpower_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t chan, int bw40, const uint8_t cck_pwr[6],
    const uint8_t ht40_pwr[5], uint8_t ht20_ofdm_diff)
{
	int group;
	uint8_t cckpow, cck2pow, htpow, ofdmpow, mcspow;
	int8_t ofdm_diff, ht20_diff;
	uint32_t reg, v4;

	if (!cck_pwr || !ht40_pwr || chan < 1 || chan > 14)
		return;

	if (chan <= 2)
		group = 0;
	else if (chan <= 5)
		group = 1;
	else if (chan <= 8)
		group = 2;
	else if (chan <= 11)
		group = 3;
	else if (chan <= 13)
		group = 4;
	else
		group = 5;

	cckpow = (cck_pwr[group] <= 0x3f) ? cck_pwr[group] : 0x2d;
	cck2pow = (cckpow >= 9) ? (uint8_t)(cckpow - 9) : 0;

	{
		uint8_t raw_ht = ht40_pwr[(group == 5) ? 4 : group];
		htpow = (raw_ht <= 0x3f) ? raw_ht : 0x2d;
	}

	ofdm_diff = (ht20_ofdm_diff == 0xff) ? 0 : sign4to8(ht20_ofdm_diff & 0x0f);
	ht20_diff = (ht20_ofdm_diff == 0xff) ? 0 : sign4to8((ht20_ofdm_diff >> 4) & 0x0f);

	ofdmpow = clamp_txpwr((int)htpow + (int)ofdm_diff);
	mcspow = clamp_txpwr((int)htpow + (bw40 ? 0 : (int)ht20_diff));

	/* CCK TXAGC (chain 0: 0xe08 + 0x86c). */
	reg = rtwn_bb_read(rd, wr, ctx, 0xe08);
	if (reg != 0xffffffffu) {
		reg = (reg & ~0x0000ff00u) | ((uint32_t)cckpow << 8);
		rtwn_bb_write(rd, wr, ctx, 0xe08, reg);
	}
	reg = rtwn_bb_read(rd, wr, ctx, 0x86c);
	if (reg != 0xffffffffu) {
		reg = (reg & 0x000000ffu) | ((uint32_t)cck2pow << 8) |
		    ((uint32_t)cckpow << 16) | ((uint32_t)cckpow << 24);
		rtwn_bb_write(rd, wr, ctx, 0x86c, reg);
	}

	/* OFDM 6..54 (0xe00, 0xe04). */
	v4 = (uint32_t)ofdmpow | ((uint32_t)ofdmpow << 8) |
	    ((uint32_t)ofdmpow << 16) | ((uint32_t)ofdmpow << 24);
	rtwn_bb_write(rd, wr, ctx, 0xe00, v4);
	rtwn_bb_write(rd, wr, ctx, 0xe04, v4);

	/* HT MCS 0..7 (0xe10, 0xe14). */
	v4 = (uint32_t)mcspow | ((uint32_t)mcspow << 8) |
	    ((uint32_t)mcspow << 16) | ((uint32_t)mcspow << 24);
	rtwn_bb_write(rd, wr, ctx, 0xe10, v4);
	rtwn_bb_write(rd, wr, ctx, 0xe14, v4);
}

/* ── Kalibrasi LO/IQ & LC (port OpenBSD rtwn.c untuk 8188E 1x1) ───── */

static int
rtwn_iqk_abs(int x)
{
	return (x < 0) ? -x : x;
}

static int
rtwn_iq_calib_chain_88e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t tx[2], uint16_t rx[2])
{
	uint32_t status;

	/* IQ calibration settings untuk chain 0 (port OpenBSD rtwn.c). */
	rtwn_bb_write(rd, wr, ctx, 0xe30, 0x10008c1fu);	/* R92C_TX_IQK_TONE_A */
	rtwn_bb_write(rd, wr, ctx, 0xe54, 0x10008c1fu);	/* R92C_RX_IQK_TONE_B */
	rtwn_bb_write(rd, wr, ctx, 0xe34, 0x10008c1fu);	/* R92C_RX_IQK_TONE_A */
	rtwn_bb_write(rd, wr, ctx, 0xe38, 0x82140102u);	/* R92C_TX_IQK_PI_A */
	rtwn_bb_write(rd, wr, ctx, 0xe3c, 0x28160502u);	/* R92C_RX_IQK_PI_A */

	rtwn_bb_write(rd, wr, ctx, 0xe4c, 0x00462911u);	/* R92C_IQK_AGC_RSP */
	rtwn_bb_write(rd, wr, ctx, 0xe48, 0xf9000000u);	/* R92C_IQK_AGC_PTS */
	rtwn_bb_write(rd, wr, ctx, 0xe48, 0xf8000000u);

	RTWN_DELAY_MS(1);

	status = rtwn_bb_read(rd, wr, ctx, 0xeac);
	if (status & (1u << 28))
		return 0;	/* TX fail */

	tx[0] = (uint16_t)((rtwn_bb_read(rd, wr, ctx, 0xe94) >> 16) & 0x3ff);
	tx[1] = (uint16_t)((rtwn_bb_read(rd, wr, ctx, 0xe9c) >> 16) & 0x3ff);
	if (tx[0] == 0x142 || tx[1] == 0x042)
		return 0;

	if (status & (1u << 27))
		return 1;	/* RX fail */

	rx[0] = (uint16_t)((rtwn_bb_read(rd, wr, ctx, 0xea4) >> 16) & 0x3ff);
	rx[1] = (uint16_t)((rtwn_bb_read(rd, wr, ctx, 0xeac) >> 16) & 0x3ff);
	if (rx[0] == 0x132 || rx[1] == 0x036)
		return 1;

	return 3;
}

void
rtwn_iq_calib_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	static const uint16_t reg_adda[16] = {
		0x85c, 0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84,
		0xe88, 0xe8c, 0xed0, 0xed4, 0xed8, 0xedc, 0xee0, 0xeec
	};
	uint32_t save_adda[16];
	uint32_t ofdm0_trxpathena = 0, ofdm0_trmuxpar = 0;
	uint32_t rfsw0 = 0, rfsw1 = 0, rfoe0 = 0, rfoe1 = 0;
	uint32_t ant_a = 0, ant_b = 0, cck_afe = 0, hssi_p1 = 0;
	uint8_t gpio_mux = 0;
	uint8_t txpause = 0, bcn_ctrl = 0, bcn_ctrl1 = 0, xa_agc = 0x20;
	uint16_t tx[3][2], rx[3][2];
	int n, i, valid = 0;

	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));
	xa_agc = (uint8_t)(rtwn_bb_read(rd, wr, ctx, 0xc50) & 0xff);

	for (n = 0; n < 3; n++) {
		if (n == 0) {
			for (i = 0; i < 16; i++)
				save_adda[i] = rtwn_bb_read(rd, wr, ctx, reg_adda[i]);
			txpause = rtwn_seq_raw_read1(rd, wr, ctx, 0x522);
			bcn_ctrl = rtwn_seq_raw_read1(rd, wr, ctx, 0x550);
			bcn_ctrl1 = rtwn_seq_raw_read1(rd, wr, ctx, 0x551);
			gpio_mux = rtwn_seq_raw_read1(rd, wr, ctx, 0x040);
			ofdm0_trxpathena = rtwn_bb_read(rd, wr, ctx, 0xc04);
			ofdm0_trmuxpar = rtwn_bb_read(rd, wr, ctx, 0xc08);
			rfsw0 = rtwn_bb_read(rd, wr, ctx, 0x870);
			rfsw1 = rtwn_bb_read(rd, wr, ctx, 0x874);
			rfoe0 = rtwn_bb_read(rd, wr, ctx, 0x860);
			rfoe1 = rtwn_bb_read(rd, wr, ctx, 0x864);
			ant_a = rtwn_bb_read(rd, wr, ctx, 0xb68);
			ant_b = rtwn_bb_read(rd, wr, ctx, 0xb6c);
			cck_afe = rtwn_bb_read(rd, wr, ctx, 0xa04);
		}

		rtwn_bb_write(rd, wr, ctx, reg_adda[0], 0x0b1b25a0u);
		for (i = 1; i < 16; i++)
			rtwn_bb_write(rd, wr, ctx, reg_adda[i], 0x0bdb25a0u);

		hssi_p1 = rtwn_bb_read(rd, wr, ctx, 0x820);
		if (!(hssi_p1 & (uint32_t)R92C_HSSI_PARAM1_PI)) {
			rtwn_bb_write(rd, wr, ctx, 0x820,
			    hssi_p1 | (uint32_t)R92C_HSSI_PARAM1_PI);
		}

		rtwn_bb_write(rd, wr, ctx, 0xc04, 0x03a05600u);
		rtwn_bb_write(rd, wr, ctx, 0xc08, 0x000800e4u);
		rtwn_bb_write(rd, wr, ctx, 0x874, 0x22204000u);

		rtwn_seq_raw_write1(rd, wr, ctx, 0x522, 0x3f);
		rtwn_seq_raw_write1(rd, wr, ctx, 0x550, (uint8_t)(bcn_ctrl & ~0x08u));
		rtwn_seq_raw_write1(rd, wr, ctx, 0x551, (uint8_t)(bcn_ctrl1 & ~0x08u));
		rtwn_seq_raw_write1(rd, wr, ctx, 0x040, (uint8_t)(gpio_mux & ~0x20u));

		rtwn_bb_write(rd, wr, ctx, 0xb68, 0x00080000u);
		rtwn_bb_write(rd, wr, ctx, 0xe28, 0x80800000u);
		rtwn_bb_write(rd, wr, ctx, 0xe40, 0x01007c00u);
		rtwn_bb_write(rd, wr, ctx, 0xe44, 0x01004800u);
		rtwn_bb_write(rd, wr, ctx, 0xb68, 0x00080000u);

		for (i = 0; i < 2; i++) {
			int ret = rtwn_iq_calib_chain_88e(rd, wr, ctx, tx[n], rx[n]);
			if (ret == 0) {
				tx[n][0] = tx[n][1] = 0xff;
				rx[n][0] = rx[n][1] = 0xff;
			} else if (ret == 1) {
				rx[n][0] = rx[n][1] = 0xff;
			}
		}

		rtwn_bb_write(rd, wr, ctx, 0xe28, 0x00000000u);
		rtwn_bb_write(rd, wr, ctx, 0x840, 0x00032ed3u);

		if (n != 0) {
			if (!(hssi_p1 & (uint32_t)R92C_HSSI_PARAM1_PI))
				rtwn_bb_write(rd, wr, ctx, 0x820, hssi_p1);
			for (i = 0; i < 16; i++)
				rtwn_bb_write(rd, wr, ctx, reg_adda[i], save_adda[i]);
			rtwn_seq_raw_write1(rd, wr, ctx, 0x522, txpause);
			rtwn_seq_raw_write1(rd, wr, ctx, 0x550, bcn_ctrl);
			rtwn_seq_raw_write1(rd, wr, ctx, 0x551, bcn_ctrl1);
			rtwn_seq_raw_write1(rd, wr, ctx, 0x040, gpio_mux);
			rtwn_bb_write(rd, wr, ctx, 0xc04, ofdm0_trxpathena);
			rtwn_bb_write(rd, wr, ctx, 0x870, rfsw0);
			rtwn_bb_write(rd, wr, ctx, 0x874, rfsw1);
			rtwn_bb_write(rd, wr, ctx, 0x860, rfoe0);
			rtwn_bb_write(rd, wr, ctx, 0x864, rfoe1);
			rtwn_bb_write(rd, wr, ctx, 0xc08, ofdm0_trmuxpar);
			rtwn_bb_write(rd, wr, ctx, 0xb68, ant_a);
			rtwn_bb_write(rd, wr, ctx, 0xb6c, ant_b);
			rtwn_bb_write(rd, wr, ctx, 0xa04, cck_afe);

			{
				uint32_t agc = rtwn_bb_read(rd, wr, ctx, 0xc50) & ~0xffu;
				rtwn_bb_write(rd, wr, ctx, 0xc50, agc | 0x50u);
				rtwn_bb_write(rd, wr, ctx, 0xc50, agc | (uint32_t)xa_agc);
			}
			rtwn_bb_write(rd, wr, ctx, 0xe30, 0x01008c00u);
			rtwn_bb_write(rd, wr, ctx, 0xe34, 0x01008c00u);

			if (tx[n - 1][0] != 0xff && tx[n][0] != 0xff &&
			    tx[n - 1][1] != 0xff && tx[n][1] != 0xff &&
			    rx[n - 1][0] != 0xff && rx[n][0] != 0xff &&
			    rx[n - 1][1] != 0xff && rx[n][1] != 0xff &&
			    rtwn_iqk_abs((int)tx[n - 1][0] - (int)tx[n][0]) <= 5 &&
			    rtwn_iqk_abs((int)tx[n - 1][1] - (int)tx[n][1]) <= 5 &&
			    rtwn_iqk_abs((int)rx[n - 1][0] - (int)rx[n][0]) <= 5 &&
			    rtwn_iqk_abs((int)rx[n - 1][1] - (int)rx[n][1]) <= 5) {
				valid = 1;
				break;
			}
		}
	}

	if (valid && tx[n][0] != 0xff && tx[n][1] != 0xff) {
		uint32_t reg, val, x;
		long y, tx_c;

		reg = rtwn_bb_read(rd, wr, ctx, 0xc80);
		val = (reg >> 22) & 0x3ff;
		x = tx[n][0];
		if (x & 0x200)
			x |= 0xfffffc00u;
		reg = (reg & ~0x3ffu) | (((x * val) >> 8) & 0x3ffu);
		rtwn_bb_write(rd, wr, ctx, 0xc80, reg);

		reg = rtwn_bb_read(rd, wr, ctx, 0xc4c);
		if (((x * val) >> 7) & 1)
			reg |= 0x80000000u;
		else
			reg &= ~0x80000000u;
		rtwn_bb_write(rd, wr, ctx, 0xc4c, reg);

		y = (long)tx[n][1];
		if (y & 0x200)
			y |= ~0x3ffL;
		tx_c = (y * (long)val) >> 8;

		reg = rtwn_bb_read(rd, wr, ctx, 0xc94);
		reg = (reg & ~0xf0000000u) | (uint32_t)((tx_c & 0x3c0) << 22);
		rtwn_bb_write(rd, wr, ctx, 0xc94, reg);

		reg = rtwn_bb_read(rd, wr, ctx, 0xc80);
		reg = (reg & ~0x003f0000u) | (uint32_t)((tx_c & 0x3f) << 16);
		rtwn_bb_write(rd, wr, ctx, 0xc80, reg);

		reg = rtwn_bb_read(rd, wr, ctx, 0xc4c);
		if (((y * (long)val) >> 7) & 1)
			reg |= 0x20000000u;
		else
			reg &= ~0x20000000u;
		rtwn_bb_write(rd, wr, ctx, 0xc4c, reg);

		if (rx[n][0] != 0xff && rx[n][1] != 0xff) {
			reg = rtwn_bb_read(rd, wr, ctx, 0xc14);
			reg = (reg & ~0x3ffu) | (rx[n][0] & 0x3ffu);
			reg = (reg & ~0xfc00u) | ((rx[n][1] & 0x03fu) << 10);
			rtwn_bb_write(rd, wr, ctx, 0xc14, reg);

			reg = rtwn_bb_read(rd, wr, ctx, 0xca0);
			reg = (reg & ~0xf0000000u) | ((rx[n][1] & 0x3c0u) << 22);
			rtwn_bb_write(rd, wr, ctx, 0xca0, reg);
		}
	}
}

void
rtwn_lc_calib_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx)
{
	uint8_t txmode;
	uint32_t rf_ac = 0, chnlbw;

	txmode = rtwn_seq_raw_read1(rd, wr, ctx, 0xd03);
	if ((txmode & 0x70) != 0) {
		rtwn_seq_raw_write1(rd, wr, ctx, 0xd03, (uint8_t)(txmode & ~0x70u));
		rf_ac = rtwn_rf_read_8188e(rd, wr, ctx, 0, 0x00);
		rtwn_rf_write_8188e(rd, wr, ctx, 0, 0x00,
		    (rf_ac & ~0x70000u) | 0x10000u);
	} else {
		rtwn_seq_raw_write1(rd, wr, ctx, 0x522, 0x3f);
	}

	chnlbw = rtwn_rf_read_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW);
	rtwn_rf_write_8188e(rd, wr, ctx, 0, R92C_RF_CHNLBW, chnlbw | 0x08000u);
	RTWN_DELAY_MS(1);

	if ((txmode & 0x70) != 0) {
		rtwn_seq_raw_write1(rd, wr, ctx, 0xd03, txmode);
		rtwn_rf_write_8188e(rd, wr, ctx, 0, 0x00, rf_ac);
	} else {
		rtwn_seq_raw_write1(rd, wr, ctx, 0x522, 0x00);
	}
}
