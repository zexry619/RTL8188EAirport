/*
 * rtwn_mp_8188e.h — tipe entri tabel parameter HAL 8188E.
 */
#ifndef RTL8188EU_RTWN_MP_8188E_H
#define RTL8188EU_RTWN_MP_8188E_H

#include <stdint.h>

typedef struct {
	uint32_t odd;	/* reg, atau descriptor bersyarat */
	uint8_t  val;	/* nilai yang ditulis */
} rtwn_mp_entry_t;

/* Arti descriptor (dari loop vendor):
 *   odd <  0x40000000 → tulis biasa: reg = odd, nilai = val
 *   odd >= 0x40000000 → entri kontrol/bersyarat (lihat driver)
 */
#define RTWN_MP_PLAIN_MAX 0x40000000u

extern const rtwn_mp_entry_t rtwn_mp_8188e_mac_reg[106];
extern const rtwn_mp_entry_t rtwn_mp_8188e_phy_reg[704];
extern const rtwn_mp_entry_t rtwn_mp_8188e_agc_tab[976];
extern const rtwn_mp_entry_t rtwn_mp_8188e_radioa[660];

#endif /* RTL8188EU_RTWN_MP_8188E_H */
