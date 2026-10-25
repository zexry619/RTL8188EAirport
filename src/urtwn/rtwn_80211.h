/*
 * rtwn_80211.h — pembangun & parser frame 802.11 (M2).
 */

#ifndef RTL8188EU_RTWN_80211_H
#define RTL8188EU_RTWN_80211_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Frame control: mgmt / probe request */
#define RTWN_FC_PROBE_REQ	0x0040
/* Frame control: mgmt / beacon */
#define RTWN_FC_BEACON		0x0080
/* Frame control: mgmt / probe response */
#define RTWN_FC_PROBE_RESP	0x0050

#define RTWN_80211_HDR_LEN	24
#define RTWN_80211_MAX_SSID	32

/* Frame control: mgmt / auth & assoc */
#define RTWN_FC_AUTH		0x00b0
#define RTWN_FC_ASSOC_REQ	0x0000
#define RTWN_FC_ASSOC_RESP	0x0010
#define RTWN_FC_DEAUTH		0x00c0
#define RTWN_FC_DATA		0x0008
#define RTWN_FC_NULL_DATA	0x0048
#define RTWN_FC_QOS_NULL	0x00c8

bool rtwn_build_probe_request(const uint8_t dst[6], const uint8_t src[6],
    const char *ssid, uint8_t ssid_len, uint8_t chan, uint8_t *out,
    size_t out_cap, size_t *out_len);

bool rtwn_parse_ssid(const uint8_t *frame, size_t frame_len, char *ssid_out,
    uint8_t *ssid_len);

bool rtwn_parse_bssid(const uint8_t *frame, size_t frame_len, uint8_t bssid[6]);

/*
 * M3 (userspace lab): auth + assoc + null-data.
 *
 * Dipakai rtwn_probe untuk membuktikan jalur TX data tanpa kernel:
 * setelah assoc, satu QoS-Null harus di-ACK AP. Kalau tidak, masalahnya
 * di descriptor/queue TX — bukan di WPA2.
 */
bool rtwn_build_auth_req(const uint8_t bssid[6], const uint8_t src[6],
    uint16_t seq, uint16_t algo, uint16_t auth_seq, uint8_t *out,
    size_t out_cap, size_t *out_len);

bool rtwn_build_assoc_req(const uint8_t bssid[6], const uint8_t src[6],
    const char *ssid, uint8_t ssid_len, uint8_t chan, bool privacy,
    uint16_t seq, uint8_t *out, size_t out_cap, size_t *out_len);

bool rtwn_build_null_data(const uint8_t bssid[6], const uint8_t src[6],
    bool qos, uint16_t seq, uint8_t *out, size_t out_cap, size_t *out_len);

/* Body auth/assoc response: status field (0 = sukses). */
bool rtwn_parse_auth_resp(const uint8_t *frame, size_t frame_len,
    uint16_t *status);
bool rtwn_parse_assoc_resp(const uint8_t *frame, size_t frame_len,
    uint16_t *status);

/* True kalau frame ini milik kita (addr1 == sta). */
bool rtwn_frame_for(const uint8_t *frame, size_t frame_len,
    const uint8_t sta[6]);

#endif /* RTL8188EU_RTWN_80211_H */
