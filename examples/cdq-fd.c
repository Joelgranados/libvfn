// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2025 The libvfn Authors. All rights reserved.
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <linux/nvme_ioctl.h>

#include "ccan/opt/opt.h"
#include "ccan/str/str.h"

#include <vfn/support.h>
#include <vfn/support/log.h>

#include "common.h"

static char *opt_mmc_bdf = "";
static char *opt_mc_bdf = "";
static bool s_usage = false;
static uint opt_size_entry_nr = 0;
static uint opt_read_entry_nr = 0;
static uint opt_zeroread_wait_ml = 0;

#define MQ_ENTRY_SIZE 32

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("--mmc-bdf", opt_set_charp, opt_show_charp, &opt_mmc_bdf,
			"Migration Manager Controller B:D:F Id"),
	OPT_WITH_ARG("--mc-bdf", opt_set_charp, opt_show_charp, &opt_mc_bdf,
			"Migratable controller B:D:F Id"),
	OPT_WITH_ARG("--size-entry-nr", opt_set_uintval, opt_show_uintval, &opt_size_entry_nr,
			"Number of 32 byte migration CDQ entries"),
	OPT_WITH_ARG("--read-entry-nr", opt_set_uintval, opt_show_uintval, &opt_read_entry_nr,
			"Number of entries to read from the CDQ before exiting"),
	OPT_WITH_ARG("--zeroread-wait-ml", opt_set_uintval, opt_show_uintval, &opt_zeroread_wait_ml,
			"millisencods to wait on zero reads. 0 means no tail pointer trigger"),
	OPT_ENDTABLE,
};

struct cdq_fd {
	int		mmc_fd;
	uint		mc_id;
	int		fd;
	int		tpt_fd;
	uint32_t	size_nbyte;
};

int cdqfd_create_eventfd(struct cdq_fd *cdq)
{
	if (cdq->tpt_fd >= 0) {
		log_error("The tft eventfd is already there\n");
		return -EINVAL;
	}

	cdq->tpt_fd = eventfd(0, EFD_CLOEXEC);
	if (cdq->tpt_fd < 0) {
		log_error("Error on eventfd creation, err : %d\n", errno);
		return -errno;
	}

	return 0;
}

int cdqfd_close_eventfd(struct cdq_fd *cdq)
{
	if (cdq->tpt_fd < 0) {
		log_error("Error closing eventfd. Invalide fd value : %d\n", cdq->tpt_fd);
		return -EINVAL;
	}

	close(cdq->tpt_fd);
	return 0;
}

static inline void cdq_zero(struct cdq_fd *cdq)
{
	cdq->mmc_fd = -1;
	cdq->mc_id = 0;
	cdq->fd = -1;
	cdq->tpt_fd = -1;
	cdq->size_nbyte = 0;
}

int cdqfd_create_ioctl_cdq(struct cdq_fd *cdq)
{
	int ret = 0;

	struct nvme_cdq_cmd cdq_cmd = {
		.size_nbyte = cdq->size_nbyte,
		.mc_id = cdq->mc_id,
	};

	ret = ioctl(cdq->mmc_fd, NVME_IOCTL_CDQ, &cdq_cmd);
	if (ret) {
		log_error("Error on cdq ioctl create, err : %d\n", errno);
		return ret;
	}

	cdq->fd = cdq_cmd.cdq_fd;

	return ret;
}

int cdqfd_delete_ioctl_cdq(struct cdq_fd *cdq)
{
	int ret = 0;
	struct nvme_cdq_cmd cdq_cmd = { .size_nbyte = 0, };

	ret = ioctl(cdq->mmc_fd, NVME_IOCTL_CDQ, &cdq_cmd);
	if (ret) {
		log_error("ERror on cdq ioctl delete, err : %d\n", errno);
		return ret;
	}

	cdq->fd = 0;

	return ret;
}

int cdqfd_alloc_cdq(struct cdq_fd **cdq_caller)
{
	struct cdq_fd *cdq;

	cdq = calloc(1, sizeof(*cdq));
	if (!cdq) {
		log_error("Error on cdq allocation, err : %d\n", errno);
		return -ENOMEM;
	}

	*cdq_caller = cdq;
	return 0;
}

void cdqfd_free_cdq(struct cdq_fd *cdq)
{
	free(cdq);
}

int cdqfd_create_cdq(struct cdq_fd **cdq_caller_ptr, const bool use_tpt,
		const uint32_t size_nbyte, const uint mc_id, const int mmc_fd)
{
	int ret = 0;
	struct cdq_fd *cdq;

	ret = cdqfd_alloc_cdq(&cdq);
	if (ret)
		goto err_out;

	cdq_zero(cdq);
	cdq->size_nbyte = size_nbyte;
	cdq->mc_id = mc_id;
	cdq->mmc_fd = mmc_fd;

	if (use_tpt) {
		ret = cdqfd_create_eventfd(cdq);
		if (ret)
			goto err_free_cdq;
	}

	ret = cdqfd_create_ioctl_cdq(cdq);
	if (ret)
		goto err_close_eventfd;

	*cdq_caller_ptr = cdq;
	return ret;

err_close_eventfd:
	if (use_tpt && cdqfd_close_eventfd(cdq))
		log_error("Error closing the eventfd\n");

err_free_cdq:
	cdqfd_free_cdq(cdq);

err_out:
	log_error("Error creating cdq, err : %d\n", ret);
	return ret;
}

int cdqfd_delete_cdq(struct cdq_fd *cdq)
{
	int ret = 0;

	if (cdq->tpt_fd > 0)
		ret |= cdqfd_close_eventfd(cdq);

	ret |= cdqfd_delete_ioctl_cdq(cdq);

	cdqfd_free_cdq(cdq);

	return ret;
}

/*
 * Return:
 *   ret <  0 : error
 *   ret == 0 : timeout
 *   ret >  0 : tpt triggered
 */
int cdqfd_wait_tptfd(struct cdq_fd *cdq, const uint wait_mili)
{
	struct pollfd pfd = {
		.fd = cdq->tpt_fd,
		.events = POLLIN,
	};

	return poll(&pfd, 1, wait_mili);
}

int cdqfd_read_cdq(struct cdq_fd *cdq, const uint read_nbytes, const uint zeroread_ml)
{
	int ret = 0;
	uint read_accum = 0;
	ssize_t ret_read;
	__autofree void *buf = NULL;

	buf = zmalloc(cdq->size_nbyte);
	if (!buf) {
		log_error("Failed to allocate read buffer err : %d\n", errno);
		ret = -ENOMEM;
		goto out;
	}

	while (read_accum < read_nbytes) {

		ret_read = read(cdq->fd, buf, read_nbytes);
		if (ret_read < 0) {
			log_error("Error on CDQ entry read\n");
			ret = ret_read;
			goto out;
		}

		if (ret_read > 0) {
			read_accum += ret_read;
			if (logv(LOG_INFO))
				hexdump(buf, ret_read, "CDQ FD read");
		}

		/* A tail pointer trigger will be activated in this case */
		if (ret_read == 0) {
			if (cdq->tpt_fd < 0)
				break; // no eventfd -> no waiting.

			ret = cdqfd_wait_tptfd(cdq, zeroread_ml);
			/* Forward the error & forward the timeout as a non-error */
			if (ret <= 0)
				goto out;
			else {
				ret = 0;
				continue;
			}
		}
	}

out:
	return ret;
}

int main(int argc, char **argv)
{
	struct cdq_fd *cdq;
	int mmc_fd = -1, mc_id, ret = EXIT_SUCCESS;
	uint64_t size_nbyte = 0, read_size_nbyte = 0;
	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(opt_mmc_bdf, ""))
		opt_usage_exit_fail("missing --mmc-bdf migration manager controller arg");
	if (streq(opt_mc_bdf, ""))
		opt_usage_exit_fail("missing --mc-bdf migratable controller arg");
	if (opt_size_entry_nr == 0)
		opt_usage_exit_fail("--entry-nr must be > 0");
	if (opt_read_entry_nr == 0)
		opt_usage_exit_fail("--read-entry-nr must be > 0");

	size_nbyte = MQ_ENTRY_SIZE * opt_size_entry_nr;
	if (size_nbyte > UINT32_MAX)
		opt_usage_exit_fail("CDQ size is too big. must be less than %ld bytes",
				UINT32_MAX);

	read_size_nbyte = MQ_ENTRY_SIZE * opt_read_entry_nr;
	if (read_size_nbyte > UINT_MAX)
		opt_usage_exit_fail("CDQ read size is too big. Must be less than %ld bytes",
				UINT_MAX);

	mc_id = get_bdf_cntl_id(opt_mc_bdf);
	if (mc_id < 0)
		opt_usage_exit_fail("Cannot get cntl id from %s", opt_mc_bdf);

	mmc_fd = get_bdf_fd(opt_mmc_bdf);
	if (mmc_fd < 0)
		opt_usage_exit_fail("Cannot open FD for %s", opt_mmc_bdf);

	opt_free_table();

	ret = cdqfd_create_cdq(&cdq, opt_zeroread_wait_ml > 0, size_nbyte, mc_id, mmc_fd);
	if (ret)
		goto err_out;

	ret = cdqfd_read_cdq(cdq, read_size_nbyte, opt_zeroread_wait_ml);
	if (ret)
		goto err_del;

	ret = cdqfd_delete_cdq(cdq);
	if (ret)
		goto err_out;

	return 0;

err_del:
	if (cdqfd_delete_cdq(cdq))
		log_error("Error Deleting CDQ on a unwind branch");

err_out:
	return ret;
}

