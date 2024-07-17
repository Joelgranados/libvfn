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

#include "linux/iommufd.h"
#include <vfn/nvme.h>
#include <vfn/pci.h>

#include <nvme/types.h>

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"

#include "common.h"
#include "vfn/vfio/pci.h"

static bool op_write, op_read;
static unsigned int op_len;
static unsigned long nsid;
static int fd;
static char* op_iopf_iova;

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
	OPT_WITH_ARG("--iopf", opt_set_charp, opt_show_charp, &op_iopf_iova, "test iopf with this iova"),
	OPT_ENDTABLE,
};

unsigned int get_lb_bytes(struct nvme_ctrl *ctrl, unsigned long nsid)
{
	void *vaddr;
	ssize_t len;
	union nvme_cmd cmd;
	struct nvme_id_ns *id_ns;
	struct nvme_lbaf *lbaf;
	unsigned int lb_bytes;

	len = pgmap(&vaddr, NVME_IDENTIFY_DATA_SIZE);
	if (len < 0)
		err(1, "could not allocate aligned memory");

	cmd.identify = (struct nvme_cmd_identify) {
		.opcode = nvme_admin_identify,
		.cns = NVME_IDENTIFY_CNS_NS,
		.nsid = cpu_to_le32(nsid),
	};

	if (nvme_admin(ctrl, &cmd, vaddr, len, NULL))
		err(1, "nvme_admin");

	id_ns = vaddr;
	lbaf = &id_ns->lbaf[id_ns->flbas];
	lb_bytes = 1ULL << lbaf->ds;

	pgunmap(vaddr, len);
	return lb_bytes;
}

int handle_iopf(struct nvme_ctrl *ctrl, void *vaddr, int op_len, uint64_t iova )
{
	int ret = 0;
	int iopf_fd = pci_get_dev_iopf_fd(&ctrl->pci);
	struct iommu_hwpt_pgfault pgfault = {0};
	ssize_t read_ret;
	uint64_t iova_tmp;

	if (iopf_fd <= 0)
		return 0;

	fprintf(stderr, "iopf_fd : %d\n", iopf_fd);

	struct iommu_hwpt_page_response pgfault_response = {
		.code = IOMMUFD_PAGE_RESP_SUCCESS,
		//.code = IOMMUFD_PAGE_RESP_INVALID,
	};

	/* Give the kenel time to process the iopf */
	sleep(1);
	read_ret = read(iopf_fd, &pgfault, sizeof(pgfault));
	if (read_ret == 0) {
		fprintf(stderr, "no iopf fd\n");
		return 0; // no page faults
	} else if (0 > read_ret) {
		fprintf(stderr, "Error reading from pagefault fd\n");
out_err:
		pgfault_response.code = IOMMUFD_PAGE_RESP_INVALID;
		ret = 1;
		goto response;
	}
	pgfault_response.cookie = pgfault.cookie;

	iova_tmp = iova;
	if (iommu_map_vaddr(__iommu_ctx(ctrl), vaddr, op_len, &iova, IOMMU_MAP_FIXED_IOVA)) {
		err(1, "failed to reserve iova");
		goto out_err;
	}

	if (iova_tmp != iova) {
		fprintf(stderr, "Different IOVAs with IOMMU_MAP_FIXED_IOVA %lx != %lx",
				iova_tmp, iova);
		goto out_err;
	}

	fprintf(stderr, "Handled iopf ::\n\tflags : %d\n\tdev_id : %d\n\t"
		"pasid : %d\n\tgrpid : %d\n\tperm : %d\n\taddr : %lld\n",
		pgfault.flags, pgfault.dev_id,
		pgfault.pasid, pgfault.grpid, pgfault.perm,
		pgfault.addr);

response:
	if (0 > write(iopf_fd, &pgfault_response, sizeof(pgfault_response))) {
		fprintf(stderr, "Error writing to pagefault fd\n");
		return 1;
	}

	return ret;
}

int main(int argc, char **argv)
{
	void *vaddr;
	uint64_t iova = 0;
	int ret;
	bool test_iopf;

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

	test_iopf = op_iopf_iova != NULL;
	if (test_iopf) {
		if (sscanf(op_iopf_iova, "%lx", &iova) != 1)
			opt_usage_exit_fail("error reading --iopf");
	}

	fprintf(stderr, "iova : %lx, test_iopf: %d, op_iopf_iova : %s\n", iova, test_iopf, op_iopf_iova);

	opt_free_table();

	if (op_len == 0)
		op_len = 0x1000;

	fd = op_read ? STDOUT_FILENO : STDIN_FILENO;

	if (nvme_init(&ctrl, bdf, &ctrl_opts))
		err(1, "failed to init nvme controller");

	unsigned int lb_nbytes = get_lb_bytes(&ctrl, nsid);

	if (nvme_create_ioqpair(&ctrl, 1, 64, -1, 0x0))
		err(1, "could not create io queue pair");

	vaddr = mmap(NULL, op_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

	if (!test_iopf) {
		if (iommu_map_vaddr(__iommu_ctx(&ctrl), vaddr, op_len, &iova, 0x0))
			err(1, "failed to reserve iova");
	}
	fprintf(stderr, "iova : %lx, vaddr: %p\n", iova, vaddr);

	if (op_write) {
		fprintf(stderr, "reading payload\n");

		ret = readmaxfd(fd, vaddr, op_len);
		if (ret < 0)
			err(1, "could not read fd");

		fprintf(stderr, "read %d bytes\n", ret);
	}

	rq = nvme_rq_acquire(&ctrl.sq[1]);

	cmd.rw = (struct nvme_cmd_rw) {
		.opcode = op_write ? nvme_cmd_write : nvme_cmd_read,
		.nsid = cpu_to_le32(nsid),
		.nlb = (ROUND_UP(op_len, lb_nbytes)/lb_nbytes) - 1,
	};

	ret = nvme_rq_map_prp(&ctrl, rq, &cmd, iova, op_len);
	if (ret)
		err(1, "could not map prps");

	nvme_rq_exec(rq, &cmd);

	if (test_iopf)
		if (handle_iopf(&ctrl, vaddr, op_len, iova))
			err(1, "error in handle_iopf");

	if (nvme_rq_spin(rq, NULL))
		err(1, "nvme_rq_poll");

	if (op_read) {
		fprintf(stderr, "writing payload\n");

		ret = writeallfd(fd, vaddr, op_len);
		if (ret < 0)
			err(1, "could not write fd");

		fprintf(stderr, "wrote %d bytes\n", ret);
	}

	nvme_rq_release(rq);

	nvme_close(&ctrl);

	fprintf(stderr, "Exiting the program...\n");
	return 0;
}
