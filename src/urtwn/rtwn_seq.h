/*
 * rtwn_seq.h — urutan inisialisasi M1 untuk RTL8188EUS, dalam C portabel.
 *
 * TUJUAN: logika ini dipakai OLEH DUA pihak:
 *   - tools/rtwn_probe.c  (user space) → bisa diuji dalam hitungan detik
 *   - src/driver/...      (kernel)      → memanggil kode yang sama
 *
 * Jadi debugging protokol TIDAK perlu kernel sama sekali.
 *
 * Semua operasi I/O lewat callback, sehingga modul ini tidak tahu apa pun
 * tentang IOKit maupun USB:
 *
 *   read (ctx, addr, buf, len)  → 0 sukses
 *   write(ctx, addr, buf, len)  → 0 sukses
 */

#ifndef RTL8188EU_RTWN_SEQ_H
#ifdef __cplusplus
extern "C" {
#endif

#define RTL8188EU_RTWN_SEQ_H

#include <stdint.h>
#include <stddef.h>

typedef int (*rtwn_read_fn)(void *ctx, uint16_t addr, void *buf, uint16_t len);
typedef int (*rtwn_write_fn)(void *ctx, uint16_t addr, const void *buf,
    uint16_t len);

/* Register Realtek (OpenBSD r92creg.h). */
#define RTWN_REG_SYS_FUNC_EN 0x002
#define RTWN_REG_APS_FSMCO   0x004
#define RTWN_REG_MCUFWDL     0x080
#define RTWN_REG_SYS_CFG     0x0f0
#define RTWN_REG_CR          0x100
#define RTWN_REG_MACID       0x610
#define RTWN_REG_EFUSE_CTRL  0x030
#define RTWN_REG_EFUSE_ACCESS 0x0cf


/*
 * Hook diagnostik.
 *
 * Lapisan portabel (C murni) tidak boleh tahu-menahu tentang IOKit.
 * Untuk diagnostik, driver mendaftarkan callback; lapisan portabel
 * cukup memanggil rtwn_seq_diag_set() dengan ctx milik driver.
 *
 * Dipakai sejak 22 Okt 2026 untuk menelusuri kegagalan baca EFUSE.
 */
typedef void (*rtwn_diag_fn)(void *ctx, const char *key, const char *val);

void rtwn_seq_diag_register(rtwn_diag_fn fn);
void rtwn_seq_diag_set(void *ctx, const char *key, const char *val);

/* Hasil tiap tahap. */
typedef struct {
	int	step;		/* tahap terakhir yang berhasil */
	int	chip_ok;	/* chip terdeteksi */
	int	efuse_ok;	/* EFUSE terbaca */
	int	power_ok;	/* power-on berhasil */
	int	dma_ok;		/* LLT/DMA terinisialisasi */
	int	fw_ok;		/* firmware 8051 terupload */
	uint8_t	mac[6];		/* MAC dari EFUSE */
	uint8_t	xtal;
	uint8_t	regulatory;
	uint8_t	channel_plan;
	uint32_t	cr;		/* register CR */
	uint32_t	mcuwdl;		/* register MCUFWDL */
	uint32_t	syscfg;
	uint32_t	fwlen;		/* panjang firmware terupload */
} rtwn_seq_result_t;

/* Langkah-langkah (urutan sama dengan OpenBSD urtwn/rtwn). */
int rtwn_seq_read_regs(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx);
int rtwn_seq_read_efuse(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx);

/* Helper level rendah (dipakai rtwn_seq_efuse.c). */
uint8_t rtwn_seq_raw_read1(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t addr);
void rtwn_seq_raw_write1(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint8_t val);
void rtwn_seq_raw_read4(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint32_t *out);
void rtwn_seq_raw_write2(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint16_t val);
void rtwn_seq_raw_write4(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx, uint16_t addr,
    uint32_t val);
void rtwn_seq_efuse_parse(const uint8_t *raw, uint16_t rawlen, uint8_t *out);

/* Power-flow vendor (tabel statik dari binary RtWlanU.kext). */
int rtwn_flow_power_on_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);
int rtwn_flow_hwpdn_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);
int rtwn_flow_radio_off_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);
int rtwn_flow_card_enable_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);

/* Init LLT 8188E (versi vendor, InitLLTTable8188E). */
int rtwn_llt_init_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint32_t page_count);

/* EFUSE 8188E dengan layout register 8188E (0x31/0x32/0x33/0x30). */
int rtwn_efuse_read1_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint16_t offset, uint8_t *out);
int rtwn_seq_read_efuse_direct_8188e(rtwn_seq_result_t *r, rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx);

/* Jalur OFFLOAD (gaya driver vendor) — untuk 8188E USB. */
int rtwn_seq_read_efuse_offload(rtwn_seq_result_t *r, rtwn_read_fn rd,
    rtwn_write_fn wr, void *ctx);
int rtwn_seq_power_on(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx);
int rtwn_seq_dma_init(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx, int ntx);
int rtwn_seq_load_firmware(rtwn_seq_result_t *r, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx, const uint8_t *fw, uint32_t fwlen);

#ifdef __cplusplus
}
#endif

#endif /* RTL8188EU_RTWN_SEQ_H */
