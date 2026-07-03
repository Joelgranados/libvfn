// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <vfn/nvme.h>
#include <vfn/pci/util.h>
#include <ctype.h>

#include "ccan/opt/opt.h"

#include "common.h"

char *bdf = "";
bool show_usage;

struct opt_table opts_base[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &show_usage, "show usage"),
	OPT_WITH_ARG("-d|--device BDF", opt_set_charp, opt_show_charp, &bdf, "pci device"),
	OPT_ENDTABLE,
};

void opt_show_ulongval_hex(char buf[OPT_SHOW_LEN], const unsigned long *ul)
{
	snprintf(buf, OPT_SHOW_LEN, "0x%lx", *ul);
}

void opt_show_uintval_hex(char buf[OPT_SHOW_LEN], const unsigned int *ui)
{
	snprintf(buf, OPT_SHOW_LEN, "0x%x", *ui);
}

void hexdump(const void *data, size_t size, const char* name)
{
	const unsigned char *byte = (const unsigned char *)data;
	size_t i, j;

	if (name)
		fprintf(stderr, "%s\n", name);

	for (i = 0; i < size; i += 16) {
		for (j = 0; j < 16; ++j) {
			if (i + j < size) {
				if (byte[i + j] != 0)
					break;
			}
		}
		if (j == 16)
			continue;

		fprintf(stderr, "%08zx  ", i);  // Offset

		// Hex bytes
		for (j = 0; j < 16; ++j) {
			if (i + j < size)
				fprintf(stderr, "%02x ", byte[i + j]);
			else
				fprintf(stderr, "   ");
		}

		fprintf(stderr, " ");

		// ASCII chars
		for (j = 0; j < 16; ++j) {
			if (i + j < size) {
				unsigned char c = byte[i + j];
				fprintf(stderr, "%c", isprint(c) ? c : '.');
			}
		}

		fprintf(stderr, "\n");
	}
}

static char *get_bdf_dev_name(const char *bdf)
{
	__autofree char *cntl = NULL;
	__autofree char *blk_name = NULL;

	blk_name = pci_get_nvme_blkname(bdf);
	if (!blk_name || asprintf(&cntl, "/dev/%s", blk_name) < 0) {
		log_debug("could not determine blk name for BDF: %s\n", bdf);
		return NULL;
	}

	return strdup(cntl);
}

int get_bdf_fd(const char *bdf)
{
	int fd;
	__autofree char *cntl = get_bdf_dev_name(bdf);
	if (!cntl) {
		log_debug("failed to find device name for bdf: %s\n", bdf);
		return -1;
	}

	fd = open(cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path (%s): %s\n",
				cntl, strerror(errno));
		return -1;
	}

	return fd;
}

int get_bdf_cntl_id(const UNUSED char* bdf)
{
	//FIXME: Actually implement this
	return 1;
}

