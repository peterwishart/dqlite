/*
 * dqlite Windows port -- <sys/random.h> shim (getrandom).
 *
 * Used by src/raft/uv.c (RNG seed) and src/vfs.c. Implemented with UCRT
 * rand_s, which draws from the OS CSPRNG (RtlGenRandom) and needs no extra
 * link libs. A BCryptGenRandom-based port can replace this later. Windows
 * ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_RANDOM_H
#define DQLITE_COMPAT_SYS_RANDOM_H

/* _CRT_RAND_S must be defined before <stdlib.h>; the build sets it project-wide
 * (see CMakeLists.txt Windows defs) so rand_s is declared. */
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#include <stdlib.h>
#include <string.h>

/* getrandom() flags (accepted and ignored -- rand_s never blocks). */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM 0x0002

static inline ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
	size_t done = 0;
	unsigned char *out = (unsigned char *)buf;
	(void)flags;
	while (done < buflen) {
		unsigned int r;
		size_t chunk;
		if (rand_s(&r) != 0) {
			return -1;
		}
		chunk = buflen - done;
		if (chunk > sizeof(r)) {
			chunk = sizeof(r);
		}
		memcpy(out + done, &r, chunk);
		done += chunk;
	}
	return (ssize_t)buflen;
}

#endif /* DQLITE_COMPAT_SYS_RANDOM_H */
