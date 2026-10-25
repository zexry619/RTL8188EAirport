/*
 * rtwn_platform_kernel.c — implementasi rtwn_sleep_ms() untuk build kext.
 *
 * Hanya file ini yang tahu soal IOKit; seluruh kode portabel di
 * src/urtwn/ tetap C murni.
 *
 * PENTING: pakai IOSleep() (memblok thread, CPU dilepas) BUKAN IODelay()
 * (busy-wait, CPU tertahan). Di kernel kext, IODelay() dalam jumlah
 * besar bisa memicu watchdog/panic.
 */

#include "rtwn_platform.h"

#if defined(KERNEL) || defined(__KERNEL__)

#include <IOKit/IOLib.h>

void rtwn_sleep_ms(uint32_t ms)
{
	if (ms == 0) {
		return;
	}
	/* IOSleep() memblok thread sambil melepas CPU untuk thread lain. */
	IOSleep(ms);
}

/*
 * Delay mikrodetik untuk timing register RF/LSSI.
 *
 * Untuk < 1 ms dipakai IODelay() — busy-wait singkat (≤ 50 µs) memang
 * diperlukan agar urutan akses RF tidak digeser penjadwal. Untuk nilai
 * besar tetap pakai IOSleep() agar tidak memblok CPU.
 */
void rtwn_delay_us(uint32_t us)
{
	if (us == 0) {
		return;
	}
	if (us >= 1000) {
		IOSleep(us / 1000);
		return;
	}
	IODelay(us);
}

#endif /* KERNEL */
