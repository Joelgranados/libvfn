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

#include <vfn/nvme.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"
#include "linux/nvme_ioctl.h"
#include "common.h"
#include "err.h"

/* FIXME: This has the double evaluation issue */
#define min(a, b) ((a < b) ? (a) : (b))
#define NVME_CDQ_MOS_CREATE_QT_UDMQ		0x0
#define NVME_ADMIN_TRACK_SEND			0x3d
#define NVME_ADMIN_SET_FEATURES			0x09
#define NVME_CDQ_SEL_LOG_USER_DATA_TRACKSEND	0x0
#define NVME_CDQ_ADM_FLAGS_TR_SEND_START	0x1
#define NVME_CDQ_ADM_FLAGS_TR_SEND_STOP		0x0
#define NVME_FEAT_CDQ_ETPT_MASK			0x80000000
#define NVME_FEAT_CDQ_ID_MASK			0xFFFF
#define NVME_FEAT_CDQ				0x21
#define NVME_CDQP_MASK				0x1
#define arm_cdq_tpt(cdq, tpt_offset) _featureid_send_cmd(cdq, tpt_offset)
#define featureid_send_cmd(cdq) _featureid_send_cmd(cdq, 0)
#define cdq_start(cdq) trsend_cmd_start(cdq)
#define cdq_output(cdq, msg) hexdump(cdq->entries, libvfn_cdq_size(cdq), msg)

static char *opt_cntl_bdf = "";
static uint cntlid = UINT_MAX;
static uint opt_entry_nbyte = 0;
static uint opt_entry_nr = 0;
static uint opt_verbose = 0;
static uint opt_max_retries = 10;
bool s_usage;

struct libvfn_cdq {
	void		*entries;
	uint32_t	curr_entry;
	uint		cdqp_offset;
	uint8_t		curr_cdqp;
	int		fd;
	uint16_t	id;
	uint32_t	entry_nbyte;
	uint32_t	entry_nr;
	uint		child_cntl_id;
	int		cntl_fd;
	int		tft_fd;
	int		epoll_fd;
};

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&opt_entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&opt_entry_nr, "Number of entries in CDQ"),
	OPT_WITH_ARG("--cntl-bdf",
			opt_set_charp, opt_show_charp,
			&opt_cntl_bdf, "Controller Bus:Device:Func Id"),
	OPT_WITH_ARG("--max-retries",
			opt_set_uintval, opt_show_uintval,
			&opt_max_retries, "Controller Bus:Device:Func Id"),
	OPT_WITH_ARG("--verbose",
			opt_set_uintval, opt_show_uintval,
			&opt_verbose, "Verbosity value"),
	OPT_WITH_ARG("--child-cntl",
			opt_set_uintval, opt_show_uintval,
			&cntlid, "Child controller to be written to"),
	OPT_ENDTABLE,
};

int _featureid_send_cmd(const struct libvfn_cdq *cdq, uint32_t tpt_offset)
{
	struct nvme_admin_cmd adm_cmd;
	struct nvme_cmd_features feat_cmd;

	feat_cmd = (struct nvme_cmd_features){
		.opcode = NVME_ADMIN_SET_FEATURES,
		.cdw12 = cpu_to_le32(cdq->curr_entry),
		.fid = cpu_to_le32(NVME_FEAT_CDQ),
		.cdw11 =  cdq->id & NVME_FEAT_CDQ_ID_MASK,
	};

	if (tpt_offset != 0) {
		/*
		 * FIXME: There is a small chance that the sent tpt will have
		 * already been handled when nvme_submit_sync_cmd returns.
		 * If we find this to be true in the CDQ, we need to send a
		 * subsequent feature_id to disable the tail pointer trigger.
		 * section 5.1.25.1.23 nvme base spec.
		 */
		feat_cmd.cdw11 |= cpu_to_le32(NVME_FEAT_CDQ_ETPT_MASK);
		feat_cmd.cdw13 = cpu_to_le32((cdq->curr_entry + tpt_offset) % cdq->entry_nr);
	}
	log_debug("%s: featuresend with tpt_offset %d\n", __func__, tpt_offset);

	memcpy(&adm_cmd, &feat_cmd, sizeof(feat_cmd));

	if (ioctl(cdq->cntl_fd, NVME_IOCTL_ADMIN_CMD, &adm_cmd)) {
		log_debug("failed sending the feature id command\n");
		return -1;
	}

	return 0;
}

/* Returns true if curr_entry forwarded by 1 */
static bool nvme_cdq_next(struct libvfn_cdq *cdq)
{
	void *curr_entry = cdq->entries + (cdq->curr_entry * cdq->entry_nbyte);
	uint8_t phase_bit = (*(uint8_t *)(curr_entry + cdq->cdqp_offset) & NVME_CDQP_MASK);
	/* if different, then its new! */
	if (phase_bit != cdq->curr_cdqp) {
		cdq->curr_entry = (cdq->curr_entry + 1) % cdq->entry_nr;
		if (unlikely(cdq->curr_entry == 0))
			cdq->curr_cdqp = ~cdq->curr_cdqp & NVME_CDQP_MASK;
		return true;
	}
	return false;
}

/*
 * Traverse the CDQ until max entries are reached or until the entry phase
 * bit is the same as the current phase bit.
 *
 * cdq : Controller Data Queue
 * count_nbyte : Count bytes to "traverse" before sending feature id
 * cdq_consume_cb : call back function. passed a buffer pointer and size.
 *                  Assume that it is contiguous
 */
size_t nvme_cdq_consume(struct libvfn_cdq *cdq, size_t count_nbyte,
			void (*cdq_consume_cb)(const void * data,
					      size_t count_nbyte,
					      const char* name))
{
	int ret;
	size_t tx_nbyte, target_nbyte = 0;
	size_t orig_tail_nbyte = (cdq->entry_nr - cdq->curr_entry) * cdq->entry_nbyte;
	void *from_buf = cdq->entries + (cdq->curr_entry * cdq->entry_nbyte);

	while (target_nbyte < count_nbyte && nvme_cdq_next(cdq))
		target_nbyte += cdq->entry_nbyte;
	tx_nbyte = min(orig_tail_nbyte, target_nbyte);

	if (cdq_consume_cb)
		cdq_consume_cb(from_buf, tx_nbyte, "nvme_cdq_consume values");

	if (tx_nbyte < target_nbyte) {
		/* Handle the entries that have been wrapped around */
		from_buf = cdq->entries;
		if (cdq_consume_cb)
			cdq_consume_cb(from_buf,  target_nbyte - tx_nbyte,
				       "nvme_cdq_consume wrapped values");
	}

	ret = featureid_send_cmd(cdq);
	if (ret < 0)
		return ret;

	return target_nbyte;
}

size_t libvfn_cdq_size(const struct libvfn_cdq *cdq)
{
	return cdq->entry_nbyte * cdq->entry_nr;
}

/* @num_zero_reads : number of consecutive zero reads. <= 0 is ignored */
int run_cdq(struct libvfn_cdq *cdq, uint rep_count, int num_zero_reads)
{
	int ret = 0, orig_num_zero_reads = num_zero_reads;
	struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};

	for (;rep_count != 0; --rep_count)
	{
		log_info("%s\n", __func__);
		nanosleep(&ts, NULL);

		ret = nvme_cdq_consume(cdq, libvfn_cdq_size(cdq), hexdump);

		if (ret < 0) {
			log_debug("failed to consume cdq\n");
			return -1;
		}

		if (ret > 0)
			num_zero_reads = orig_num_zero_reads;

		if (orig_num_zero_reads > 0 && ret == 0) {
			if (num_zero_reads-- == 0)
				break;
		}
	}

	/*
	 * At some point we can return the retries that were not executed.
	 * For now we do not need them
	 */
	//return rep_count;
	return 0;
}

int trsend_cmd_start(const struct libvfn_cdq *cdq)
{
	struct nvme_admin_cmd cmd;
	struct nvme_cmd_cdq cdq_cmd;

	cdq_cmd = (struct nvme_cmd_cdq) {
		   .opcode = NVME_ADMIN_TRACK_SEND,
		   .sel = NVME_CDQ_SEL_LOG_USER_DATA_TRACKSEND,
		   .mos = cpu_to_le16(NVME_CDQ_ADM_FLAGS_TR_SEND_START),
		   .cdq_id = cpu_to_le16(cdq->id)
	};

	memcpy(&cmd, &cdq_cmd, sizeof(cdq_cmd));

	if (ioctl(cdq->cntl_fd, NVME_IOCTL_ADMIN_CMD, &cmd)) {
		log_debug("failed sending the track command for cdq: %d\n", cdq->id);
		return -1;
	}

	return 0;
}

int create_cdq_epoll(const int tft_fd)
{
	struct epoll_event ev;
	ev.events = EPOLLIN;
	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd == -1) {
		log_error("Error creating the epoll file descriptor. Errno %d\n", errno);
		return -1;
	}

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tft_fd, &ev) == -1) {
		log_error("Error adding %d to the epoll file descriptor. Errno %d\n",
			  tft_fd, errno);
		close (epoll_fd);
		return -1;
	}

	return epoll_fd;
}

int setup_cdq_kernel(struct libvfn_cdq *cdq)
{
	int ret = 0;
	struct nvme_cdq_cmd cdq_cmd = {};

	cdq_cmd.size_nbyte = libvfn_cdq_size(cdq);
	cdq_cmd.entries = (unsigned long)cdq->entries;
	cdq_cmd.cqs = cdq->child_cntl_id;
	cdq_cmd.mos = NVME_CDQ_MOS_CREATE_QT_UDMQ;
	if (cdq->tft_fd > 0)
		cdq_cmd.tpt_fd = cdq->tft_fd;

	if (ioctl(cdq->cntl_fd, NVME_IOCTL_CDQ, &cdq_cmd)) {
		log_debug("setup_cdq_kernel: failed on NVME_IOCTL_CDQ\n");
		ret = -1;
		goto out;
	}

	cdq->id = cdq_cmd.id;

out:
	return ret;
}

int teardown_cdq_kernel(struct libvfn_cdq *cdq)
{
	struct nvme_cdq_cmd cdq_cmd = {
		.size_nbyte = 0,
		.id = cdq->id,
	};

	if (ioctl(cdq->cntl_fd, NVME_IOCTL_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_IOCTL_CDQ\n");
		return -1;
	}

	return 0;
}

	void		*entries;
	uint32_t	curr_entry;
	uint		cdqp_offset;
	uint8_t		curr_cdqp;
	int		fd;
	uint16_t	id;
	uint32_t	entry_nbyte;
	uint32_t	entry_nr;
	uint		child_cntl_id;
	int		cntl_fd;
	int		tft_fd;
	int		epoll_fd;

void zero_libvfn_cdq(struct libvfn_cdq *cdq)
{
	cdq->entries = NULL;
	cdq->curr_entry = 0;
	cdq->cdqp_offset = 0;
	cdq-> curr_cdqp = 0;
	cdq->fd = -1;
	cdq->id = 0;
	cdq->entry_nbyte = 0;
	cdq->entry_nr = 0;
	cdq->child_cntl_id = 0;
	cdq->cntl_fd = -1;
	cdq->tft_fd = -1;
	cdq->epoll_fd = -1;
}

int cdq_attach_eventfd(struct libvfn_cdq *cdq)
{
	if (cdq->tft_fd >= 0) {
		log_error("The tft eventfd is already there\n");
		return -1;
	}
	cdq->tft_fd = eventfd(0, EFD_CLOEXEC);
	if (cdq->tft_fd < 0) {
		log_error("Error on eventfd creation, err: %d\n", errno);
		return -1;
	}
	return 0;
}

int cdq_attach_epoll(struct libvfn_cdq *cdq)
{
	if (cdq->tft_fd < 0) {
		log_error("Missing the eventfd file descriptor\n");
		return -1;
	}
	if (cdq->epoll_fd >= 0) {
		log_error("The epoll_fd is already there\n");
		return -1;
	}

	cdq->epoll_fd = create_cdq_epoll(cdq->tft_fd);
	if (cdq->epoll_fd <  0) {
		log_error("Error on epoll creation, err: %d\n", errno);
		return -1;
	}
	return 0;
}

int cdq_map_entries(struct libvfn_cdq *cdq)
{
	if (cdq->entries) {
		log_error("Already have allocated entries\n");
		return -1;
	}

	cdq->entries = mmap(NULL, libvfn_cdq_size(cdq), PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (!cdq->entries) {
		log_error("Failed to mmap the cdq into user space, err: %d\n", errno);
		return -1;
	}
	return 0;
}

int cdq_tpt_wait(struct libvfn_cdq *cdq, const uint32_t tpt_offset, const int timeout)
{
	size_t s;
	int ret = 0;
	uint64_t e_fd_v;
	struct epoll_event e_events;

	/* 10 is arbitrary */
	ret = arm_cdq_tpt(cdq, tpt_offset);
	if (ret)
		return -1;

	if (epoll_wait(cdq->epoll_fd, &e_events, 1, timeout) == 0) {
		log_info("did not receive an event from the eventfd file descriptor\n");
		return -1;
	}

	s = read(cdq->tft_fd, &e_fd_v, sizeof(uint64_t));
	if (s != sizeof(uint64_t)) {
		ret = -1;
		log_error("Failed to read eventfd file descriptor variable");
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int ret = 0;
	struct libvfn_cdq cdq = {};

	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(opt_cntl_bdf, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");
	if (opt_entry_nbyte == 0 || opt_entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");
	if (cntlid == UINT_MAX)
		opt_usage_exit_fail("must pass at least one --child-cntl");

	opt_free_table();

	zero_libvfn_cdq(&cdq);

	cdq.entry_nbyte = opt_entry_nbyte;
	cdq.entry_nr = opt_entry_nr;
	cdq.child_cntl_id = cntlid;
	cdq.cdqp_offset = cdq.entry_nbyte - 1; // Guess that its in the last bit

	cdq.cntl_fd = get_bdf_fd(opt_cntl_bdf);
	if (cdq.cntl_fd < 0) {
		ret = -1;
		errno = ret;
		log_error("Error getting fd for %s\n", opt_cntl_bdf);
		goto out_err;
	}

	ret = cdq_attach_eventfd(&cdq);
	if (ret)
		goto out_err;

	ret = cdq_attach_epoll(&cdq);
	if (ret)
		goto close_eventfd;

	ret = cdq_map_entries(&cdq);
	if (ret)
		goto close_epoll;

	ret = setup_cdq_kernel(&cdq);
	if (ret)
		goto unmap_entries;

	ret = cdq_start(&cdq);
	if (ret)
		goto del_cdq;

	cdq_output((&cdq), "Initial CDQ Value");

	ret = run_cdq(&cdq, opt_max_retries, 1);
	if (ret)
		goto del_cdq;

	/* 10 is arbitrary */
	ret = cdq_tpt_wait(&cdq, 10, 10000);
	if (ret)
		goto del_cdq;

	ret = run_cdq(&cdq, opt_max_retries, 0);
	if (ret)
		goto del_cdq;

del_cdq:
	ret |= teardown_cdq_kernel(&cdq);

unmap_entries:
	ret |= munmap(cdq.entries, libvfn_cdq_size(&cdq));

close_epoll:
	close(cdq.epoll_fd);

close_eventfd:
	close(cdq.tft_fd);

out_err:
	if (ret)
		err(EXIT_FAILURE, NULL);

	exit(ret);
}
