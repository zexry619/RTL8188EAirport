/*
 * rtwn_platform.h — lapisan platform tipis untuk kode portabel.
 *
 * Hanya ada satu hal yang benar-benar berbeda antara kernel (kext
 * macOS) dan user space: cara menunggu/menunda waktu.
 *
 * ‑‑‑ MASALAH SERIUS YANG INI SELESAIKAN ‑‑‑
 *
 * Sebelumnya semua file memakai:
 *
 *     #define RTWN_DELAY_MS(ms) ((void)(ms))
 *
 * yaitu NO-OP. Di build kernel artinya setiap loop "tunggu chip" berubah
 * menjadi spin USB tanpa jeda:
 *
 *   - rtwn_efuse_8188e.c : 100 iterasi × (1000 ms yang diabaikan)
 *   - rtwn_seq_efuse.c   : 1000 iterasi × (50 ms yang diabaikan)
 *   - rtwn_flow.c        : loop POLL tanpa jeda
 *
 * Akibatnya:
 *   1. Chip belum selesai → register dibaca STALE → data EFUSE salah.
 *   2. Thread kernel tidak pernah yield → workloop kext membeku
 *      (kernel hang yangALTHsafe karena tidak bisa di-interrupt).
 *   3. Memperbaiki dengan IODelay() juga salah: IODelay = busy-wait
 *      tanpa yield. 1000 × IODelay(50 ms) = 50 detik busy-wait.
 *
 * Di kernel kita harus MEMBLOK thread (IOSleep), bukan membekukan CPU.
 * Di user space cukup usleep().
 */

#ifndef RTWN_PLATFORM_H
#define RTWN_PLATFORM_H

#include <stdint.h>

#if defined(KERNEL) || defined(__KERNEL__)

/*
 * Implementasi lived di rtwn_platform_kernel.c (hanya dikompilasi ke
 * kext). Fungsi ini memanggil IOSleep() → threadYield, CPU tetap bisa
 * dipakai thread lain.
 */
void rtwn_sleep_ms(uint32_t ms);
void rtwn_delay_us(uint32_t us);

#define RTWN_DELAY_MS(ms) rtwn_sleep_ms((uint32_t)(ms))
#define RTWN_DELAY_US(us) rtwn_delay_us((uint32_t)(us))

#elif defined(__APPLE__)

#include <unistd.h>
#define RTWN_DELAY_MS(ms) usleep((useconds_t)((ms) * 1000))
#define RTWN_DELAY_US(us) usleep((useconds_t)(us))

#else

#define RTWN_DELAY_MS(ms) ((void)(ms))
#define RTWN_DELAY_US(us) ((void)(us))

#endif

#endif /* RTWN_PLATFORM_H */
