// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#include <errno.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/mman.h>

#include <vfn/support/align.h>
#include <vfn/support/compiler.h>
#include <vfn/support/mem.h>

long __VFN_PAGESIZE;
int __VFN_PAGESHIFT;

static void __attribute__((constructor)) init_page_size(void)
{
	__VFN_PAGESIZE = sysconf(_SC_PAGESIZE);
	__VFN_PAGESHIFT = 31 - __builtin_clz(__VFN_PAGESIZE);
}

void backtrace_abort(void)
{
#ifdef __GLIBC__
	void *buf[10];
	char **symbols = NULL;
	int size;

	size = backtrace(buf, 10);
	symbols = backtrace_symbols(buf, size);

	if (symbols) {
		fprintf(stderr, "fatal error; dumping maximum %d stack frames\n", size);

		for (int i = 0; i < size; i++)
			fprintf(stderr, "[%d]: %s\n", i, symbols[i]);
	}

	free(symbols);
#endif /* __GLIBC__ */
	abort();
}

ssize_t pgmap(void **mem, size_t sz)
{
	ssize_t len = ALIGN_UP(sz, __VFN_PAGESIZE);

	*mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (*mem == MAP_FAILED)
		return -1;

	return len;
}

ssize_t pgmapn(void **mem, unsigned int n, size_t sz)
{
	if (would_overflow(n, sz)) {
		fprintf(stderr, "allocation of %d * %zu bytes would overflow\n", n, sz);

		backtrace_abort();
	}

	return pgmap(mem, n * sz);
}


