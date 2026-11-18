/*
 * rtwn_chan.h — pengaturan kanal & akses register RF/BB 8188E.
 *
 * Port dari OpenBSD rtwn.c (ISC).
 */

#ifndef RTL8188EU_RTWN_CHAN_H
#define RTL8188EU_RTWN_CHAN_H

#include <stdint.h>
#include <stdbool.h>

#include "rtwn_seq.h"
#include "rtwn_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t rtwn_rf_read_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    int chain, uint8_t addr);
int      rtwn_rf_write_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    int chain, uint16_t addr, uint32_t val);
int      rtwn_set_chan_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t chan);
int      rtwn_set_chan_bw_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t chan, int bw40, int prichlo);
void     rtwn_set_rx_gain_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t gain);
void     rtwn_set_txpower_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t chan, int bw40, const uint8_t cck_pwr[6],
    const uint8_t ht40_pwr[5], uint8_t ht20_ofdm_diff);
void     rtwn_iq_calib_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);
void     rtwn_lc_calib_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RTL8188EU_RTWN_CHAN_H */
