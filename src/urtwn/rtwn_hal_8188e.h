/*
 * rtwn_hal_8188e.h — konteks & API penerapan tabel HAL 8188E.
 */

#ifndef RTL8188EU_RTWN_HAL_8188E_H
#define RTL8188EU_RTWN_HAL_8188E_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rtwn_seq.h"		/* rtwn_read_fn / rtwn_write_fn */
#include "rtwn_mp_8188e.h"	/* rtwn_mp_entry_t */

typedef struct {
	rtwn_read_fn	rd;
	rtwn_write_fn	wr;
	void	       *ctx;

	/*
	 * "MAC window" 8 byte: 6 byte MAC + 2 byte.
	 * Dipakai untuk cabang bersyarat pada entri 0x4xxxxxxx.
	 */
	uint8_t		macwin[8];
} rtwn_hal_ctx_t;

void rtwn_hal_init(rtwn_hal_ctx_t *hc, rtwn_read_fn rd, rtwn_write_fn wr,
    void *ctx);
void rtwn_hal_set_mac(rtwn_hal_ctx_t *hc, const uint8_t mac[6]);
int  rtwn_hal_apply_table(rtwn_hal_ctx_t *hc, const rtwn_mp_entry_t *tab,
    size_t n);

#endif /* RTL8188EU_RTWN_HAL_8188E_H */
