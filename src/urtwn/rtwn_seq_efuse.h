/*
 * rtwn_seq_efuse.h — API internal jalur EFUSE (untuk pengujian offline).
 */

#ifndef RTL8188EU_RTWN_SEQ_EFUSE_H
#define RTL8188EU_RTWN_SEQ_EFUSE_H

#include <stdint.h>

#include "rtwn_seq.h"

/*
 * Gaya pause/enable 8051 —eze according to vendor HAL_8051*Wrapper,
 * which chooses the register based on cardType.
 *
 *   STYLE_A : cardType 0x0e (8188E)  -> 0x1c / 0x1d / 0x03
 *   STYLE_B : cardType 0x0f / 0x10    -> 0x1c / 0x1d / 0x03
 *   STYLE_32: cardType 0x32            -> 0x93 saja
 */
#define RTWN_8051_STYLE_A	1
#define RTWN_8051_STYLE_B	2
#define RTWN_8051_STYLE_32	0

void rtwn_8051_pause_test(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    int style);
void rtwn_8051_enable_test(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    int style);

#endif /* RTL8188EU_RTWN_SEQ_EFUSE_H */
