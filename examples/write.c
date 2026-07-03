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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"

#include "common.h"

static unsigned long nsid;
static unsigned long seed;
static unsigned long writes_num = 1;
static bool journal_write;

#define JOURNAL_DIR "/tmp/libvfnW"
#define JOURNAL_MAX_BYTES 512

static const struct nvme_ctrl_opts ctrl_opts = {
	.nsqr = 63,
	.ncqr = 63,
};

static struct opt_table opts[] = {
	OPT_SUBTABLE(opts_base, NULL),
	OPT_WITH_ARG("-N|--nsid", opt_set_ulongval, opt_show_ulongval, &nsid,
		     "namespace identifier"),
	OPT_WITH_ARG("-s|--seed", opt_set_ulongval, opt_show_ulongval, &seed,
		     "random seed (default: time-based)"),
	OPT_WITHOUT_ARG("--journal-write", opt_set_bool, &journal_write,
		     "Write data to /tmp/libvfnW/SLBA after each write command"),
	OPT_WITH_ARG("-n|--writes-num", opt_set_ulongval, opt_show_ulongval, &writes_num,
		     "number of write operations to perform (default: 1)"),
	OPT_ENDTABLE,
};

static void fill_random_pattern(void *buf, size_t len)
{
	uint32_t *p = buf;
	size_t i;

	for (i = 0; i < len / sizeof(uint32_t); i++)
		p[i] = (uint32_t)rand();
}

static uint64_t get_namespace_size(struct nvme_ctrl *ctrl, uint32_t ns_id)
{
	struct nvme_id_ns *id_ns;
	union nvme_cmd cmd;
	void *vaddr;
	ssize_t len;
	uint64_t nsze;

	len = pgmap(&vaddr, NVME_IDENTIFY_DATA_SIZE);
	if (len < 0)
		err(1, "could not allocate aligned memory for identify");

	cmd.identify = (struct nvme_cmd_identify) {
		.opcode = nvme_admin_identify,
		.cns = NVME_IDENTIFY_CNS_NS,
		.nsid = cpu_to_le32(ns_id),
	};

	if (nvme_admin(ctrl, &cmd, vaddr, len, NULL))
		err(1, "failed to identify namespace");

	id_ns = vaddr;
	nsze = le64_to_cpu(id_ns->nsze);

	pgunmap(vaddr, len);

	return nsze;
}

static uint64_t generate_random_slba(struct nvme_ctrl *ctrl, uint32_t ns_id)
{
	uint64_t nsze;
	uint64_t slba;

	nsze = get_namespace_size(ctrl, ns_id);
	if (nsze == 0)
		err(1, "namespace size is zero");

	/* Generate random SLBA using two rand() calls for better distribution */
	slba = ((uint64_t)rand() << 32) | (uint64_t)rand();
	slba = slba % nsze;

	return slba;
}

static int journal_write_data(uint64_t slba, void *buf, size_t len)
{
	char path[64];
	unsigned char data[JOURNAL_MAX_BYTES];
	size_t write_len;
	int fd;

	if (mkdir(JOURNAL_DIR, 0755) && errno != EEXIST) {
		warn("failed to create journal directory");
		return -1;
	}

	snprintf(path, sizeof(path), JOURNAL_DIR "/%lu", (unsigned long)slba);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		warn("failed to open journal file %s", path);
		return -1;
	}

	memset(data, 0, JOURNAL_MAX_BYTES);
	write_len = len < JOURNAL_MAX_BYTES ? len : JOURNAL_MAX_BYTES;
	memcpy(data, buf, write_len);

	if (write(fd, data, JOURNAL_MAX_BYTES) != JOURNAL_MAX_BYTES) {
		warn("failed to write journal file %s", path);
		close(fd);
		return -1;
	}

	close(fd);

	log_info("journaled %zu bytes to %s\n", write_len, path);

	return 0;
}

static void nvme_write(struct nvme_ctrl *ctrl, void *vaddr, uint64_t iova)
{
	struct nvme_rq *rq;
	union nvme_cmd cmd;
	uint64_t slba;
	int ret;

	slba = generate_random_slba(ctrl, nsid);
	log_info("random slba: %lu\n", (unsigned long)slba);

	fill_random_pattern(vaddr, 0x1000);

	rq = nvme_rq_acquire(&ctrl->sq[1]);

	cmd.rw = (struct nvme_cmd_rw) {
		.opcode = nvme_cmd_write,
		.nsid = cpu_to_le32(nsid),
		.slba = cpu_to_le64(slba),
	};

	ret = nvme_rq_map_prp(ctrl, rq, &cmd, iova, 0x1000);
	if (ret)
		err(1, "could not map prps");

	nvme_rq_exec(rq, &cmd);

	if (nvme_rq_spin(rq, NULL))
		err(1, "nvme_rq_poll");

	log_info("write completed successfully\n");

	if (journal_write)
		journal_write_data(slba, vaddr, 0x1000);

	nvme_rq_release(rq);
}

int main(int argc, char **argv)
{
	void *vaddr;
	uint64_t iova;
	unsigned long i;

	struct nvme_ctrl ctrl = {};

	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (show_usage)
		opt_usage_and_exit(NULL);

	if (streq(bdf, ""))
		opt_usage_exit_fail("missing --device parameter");

	opt_free_table();

	if (seed == 0)
		seed = (unsigned long)time(NULL);

	srand((unsigned int)seed);
	log_info("using random seed: %lu\n", seed);

	if (nvme_init(&ctrl, bdf, &ctrl_opts))
		err(1, "failed to init nvme controller");

	if (nvme_create_ioqpair(&ctrl, 1, 64, -1, 0x0))
		err(1, "could not create io queue pair");

	vaddr = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

	if (iommu_map_vaddr(__iommu_ctx(&ctrl), vaddr, 0x1000, &iova, 0x0))
		err(1, "failed to reserve iova");

	for (i = 0; i < writes_num; i++)
		nvme_write(&ctrl, vaddr, iova);

	nvme_close(&ctrl);

	return 0;
}
