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

#include <nvme/types.h>

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"

#include "common.h"

static bool op_write, op_read;
static unsigned int op_len;
static unsigned long nsid;
static int fd;

static const struct nvme_ctrl_opts ctrl_opts = {
	.nsqr = 63,
	.ncqr = 63,
};

static struct opt_table opts[] = {
	OPT_SUBTABLE(opts_base, NULL),
	OPT_WITH_ARG("-N|--nsid", opt_set_ulongval, opt_show_ulongval, &nsid, "namespace identifier"),
	OPT_WITHOUT_ARG("-w|--write", opt_set_bool, &op_write, "perform a write"),
	OPT_WITHOUT_ARG("-r|--read", opt_set_bool, &op_read, "perform a read"),
	OPT_WITH_ARG("-z|--size", opt_set_uintval_bi, opt_show_uintval_bi, &op_len, "size of operation"),
	OPT_ENDTABLE,
};

int main(int argc, char **argv)
{
	void *vaddr;
	uint64_t iova;
	int ret;

	struct nvme_ctrl ctrl = {};
	struct nvme_rq *rq;
	union nvme_cmd cmd;

	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (show_usage)
		opt_usage_and_exit(NULL);

	if (streq(bdf, ""))
		opt_usage_exit_fail("missing --device parameter");

	if (op_write == op_read)
		opt_usage_exit_fail("specify one of -r or -w");

	opt_free_table();

	fd = op_read ? STDOUT_FILENO : STDIN_FILENO;

	if (nvme_init(&ctrl, bdf, &ctrl_opts))
		err(1, "failed to init nvme controller");

	if (nvme_create_ioqpair(&ctrl, 1, 64, 0x0))
		err(1, "could not create io queue pair");

	vaddr = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

	if (vfio_map_vaddr(ctrl.pci.dev.vfio, vaddr, 0x1000, &iova))
		err(1, "failed to reserve iova");

	if (op_write) {
		fprintf(stderr, "reading payload\n");

		ret = readmaxfd(fd, vaddr, 0x1000);
		if (ret < 0)
			err(1, "could not read fd");

		fprintf(stderr, "read %d bytes\n", ret);
	}

	rq = nvme_rq_acquire(&ctrl.sq[1]);

	cmd.rw = (struct nvme_cmd_rw) {
		.opcode = op_write ? nvme_cmd_write : nvme_cmd_read,
		.nsid = cpu_to_le32(nsid),
	};

	nvme_rq_map_prp(rq, &cmd, iova, 0x1000);

	nvme_rq_exec(rq, &cmd);

	if (nvme_rq_poll(rq, NULL))
		err(1, "nvme_rq_poll");

	if (op_read) {
		fprintf(stderr, "writing payload\n");

		ret = writeallfd(fd, vaddr, 0x1000);
		if (ret < 0)
			err(1, "could not write fd");

		fprintf(stderr, "wrote %d bytes\n", ret);
	}

	nvme_rq_release(rq);

	nvme_close(&ctrl);

	return 0;
}
