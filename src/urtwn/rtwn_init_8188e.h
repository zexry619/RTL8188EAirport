/*
 * rtwn_init_8188e.h — inisialisasi MAC/BB/RF RTL8188E (M2).
 *
 * Port dari OpenBSD urtwn (ISC):
 *   rtwn_init()        sys/dev/ic/rtwn.c
 *   urtwn_mac_init()   sys/dev/usb/if_urtwn.c
 *   urtwn_bb_init()    sys/dev/usb/if_urtwn.c
 *   rtwn_rf_init()     sys/dev/ic/rtwn.c
 *   urtwn_aggr_init()  sys/dev/usb/if_urtwn.c
 *
 * Tabel init (MAC / BB / AGC / RF) diambil langsung dari r92creg.h —
 * tabel yang sama yang dipakai driver OpenBSD untuk 8188EU, jadi tidak
 * perlu menebak isi tabel vendor.
 */

#ifndef RTL8188EU_RTWN_INIT_8188E_H
#define RTL8188EU_RTWN_INIT_8188E_H

#include <stdint.h>

#include "rtwn_seq.h"

/*
 * Inisialisasi blok MAC (tabel rtl8188eu_mac).
 * Return jumlah register yang ditulis.
 */
int rtwn_init_mac_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);

/*
 * Inisialisasi blok BB + AGC + crystal cap.
 * @xtal : nilai crystal trim (6 bit), dari EFUSE.
 * Return jumlah register yang ditulis.
 */
int rtwn_init_bb_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    uint8_t xtal);

/*
 * Inisialisasi blok RF chain 0 (1T1R).
 * Return jumlah register RF yang ditulis.
 */
int rtwn_init_rf_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx);

/*
 * Konfigurasi inti setelah MAC/BB/RF:
 *   - alamat MAC ke register MACID
 *   - Rx filter (RCR/MAR/RXFLTMAP)
 *   - EDCA / SIFS / ACK timeout
 *   - SECCFG, HWSEQ
 *   - agregasi USB (RXDMA_AGG)
 *   - TX enable + TX report
 *
 * @mac : MAC 6 byte (dari EFUSE)
 */
int rtwn_core_config_8188e(rtwn_read_fn rd, rtwn_write_fn wr, void *ctx,
    const uint8_t mac[6]);

#endif /* RTL8188EU_RTWN_INIT_8188E_H */
