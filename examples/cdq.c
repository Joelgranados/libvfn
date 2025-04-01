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

#include <vfn/support.h>
#include <sys/ioctl.h>
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"
#include "linux/nvme_ioctl.h"

static char *parent_cntl = "";
static unsigned int cntlid = 0;
static unsigned int entry_nbyte = 0;
static unsigned int entry_nr = 0;
bool s_usage;

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("-C|--child-cntl",
			opt_set_uintval, opt_show_uintval,
			&cntlid, "Child controller ID"),
	OPT_WITH_ARG("-P|--parent-cntl",
			opt_set_charp, opt_show_charp,
			&parent_cntl, "Parent controller device path"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&entry_nr, "Number of entries in CDQ"),
	OPT_ENDTABLE,
};

int main(int argc, char **argv)
{
	int fd;
	struct nvme_cdq_cmd cdq_cmd;

	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(parent_cntl, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	opt_free_table();

	fprintf(stderr, "child cntl : %d\n", cntlid);
	fprintf(stderr, "parent path : %s\n", parent_cntl);

	fd = open(parent_cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	cdq_cmd.cntlid = cntlid;
	cdq_cmd.entry_nbyte = entry_nbyte;
	cdq_cmd.entry_nr = entry_nr;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ_ALLOC, &cdq_cmd)) {
		log_debug("failed on NVME_IOCTL_ADMIN_CDQ_ALLOC");
		return -1;
	}

	return 0;
}
