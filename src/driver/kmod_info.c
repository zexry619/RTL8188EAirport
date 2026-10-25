/*
 * kmod_info.c — struktur kmod_info untuk kext.
 *
 * Kernel macOS (libkern/kxld) mencari simbol "_kmod_info" di dalam binary kext.
 * Xcode membuat file ini secara otomatis lewat CreateKModInfo.perl; karena
 * proyek ini dibangun tanpa Xcode, kita generates sendiri.
 *
 * Layout struct mengikuti XNU (osfmk/mach/kmod.h):
 *   0x00 next              (8)
 *   0x08 info_version      (4)   = 1
 *   0x0c id                (4)
 *   0x10 name[64]
 *   0x50 version[64]
 *   0x90 reference_count   (4)
 *   0x94 reference_list    (8)
 *   0x9c address           (8)
 *   0xa4 size              (8)
 *   0xac hdr_size          (8)
 *   0xb4 start             (8)
 *   0xbc stop              (8)
 *
 * _start/_stop berasal dari libkmod (MacKernelSDK/Library/x86_64/libkmod.a);
 * keduanya memanggil _realmain/_antimain bila diisi. Kext ini tidak butuh
 * main khusus, jadi keduanya dibiarkan NULL.
 *
 * Lisensi: GPL-2.0-or-later (lihat ../../LICENSES.md)
 */

#include <mach/kmod.h>

/* Dipanggil oleh libkmod (c_start.c / c_stop.c). */
kmod_start_func_t *_realmain = 0;
kmod_stop_func_t  *_antimain = 0;

/* start/stop dari libkmod. */
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

kmod_info_t kmod_info = {
	0,					/* next */
	KMOD_INFO_VERSION,			/* info_version */
	0xFFFFFFFF,				/* id */
	"io.github.zekri.RTL8188EAirport",	/* name */
	"0.9.1",				/* version */
	-1,					/* reference_count */
	0,				/* reference_list */
	0,				/* address */
	0,				/* size */
	0,				/* hdr_size */
	_start,				/* start */
	_stop				/* stop */
};
