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

#include <stdint.h>
#include <vfn/support.h>
#include <vfn/pci.h>
#include <vfn/nvme.h>
#include <sys/ioctl.h>
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"
#include "linux/nvme_ioctl.h"

static char *cntl_bdf = "";
static char *action = "";
static unsigned int cntlid = UINT_MAX;
static unsigned int cdqid = UINT_MAX;
static unsigned int entry_nbyte = 0;
static unsigned int entry_nr = 0;
static unsigned int verbose = 0;
bool s_usage;

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("-C|--child-cntl",
			opt_set_uintval, opt_show_uintval,
			&cntlid, "Child controller ID"),
	OPT_WITH_ARG("--cdq-id",
			opt_set_uintval, opt_show_uintval,
			&cdqid, "Controller Data Queue Identifier"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&entry_nr, "Number of entries in CDQ"),
	OPT_WITH_ARG("--cntl-bdf",
			opt_set_charp, opt_show_charp,
			&cntl_bdf, "Controller Bus:Device:Func Id"),
	OPT_WITH_ARG("-A|--action",
			opt_set_charp, opt_show_charp,
			&action, "Action to perform: create{&read}, tr_send_start, delete"),
	OPT_WITH_ARG("--verbose",
			opt_set_uintval, opt_show_uintval,
			&verbose, "Verbosity value"),
	OPT_ENDTABLE,
};

void hexdump(const void *data, size_t size) {
	const unsigned char *byte = (const unsigned char *)data;
	size_t i, j;

	for (i = 0; i < size; i += 16) {
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

int get_bdf_fd(const char *bdf)
{
	int fd;
	__autofree char *cntl = NULL;
	__autofree char *blk_name = NULL;

	blk_name = pci_get_nvme_blkname(bdf);
	if (!blk_name || asprintf(&cntl, "/dev/%s", blk_name) < 0) {
		log_debug("could not determine blk name for BDF: %s\n", bdf);
		return -1;
	}

	fd = open(cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	if (verbose > 0) {
		fprintf(stderr, "parent path : %s\n", cntl);
	}


	return fd;
}

#define NVME_CDQ_MOS_CREATE_QT_UDMQ	0x0
int do_action_create(uint16_t *cdqid, int *readfd)
{
	int fd, ret = 0;
	struct nvme_cdq_cmd cdq_cmd;

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	if (cntlid == UINT_MAX)
		opt_usage_exit_fail("missing controller id");

	if (verbose > 0)
		fprintf(stderr, "child cntl : %d\n", cntlid);

	fd = get_bdf_fd(cntl_bdf);
	if (fd < 0)
		return -1;

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_CREATE;
	cdq_cmd.adm.entry_nbyte = entry_nbyte;
	cdq_cmd.adm.entry_nr = entry_nr;
	cdq_cmd.adm.cqs = cntlid;
	cdq_cmd.adm.mos = NVME_CDQ_MOS_CREATE_QT_UDMQ;

	// Guess that its in the last bit
	cdq_cmd.adm.cdqp_offset = entry_nbyte;
	cdq_cmd.adm.cdqp_mask = 0x1;

	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_CREATE");
		ret = -1;
		goto out;
	}

	*cdqid = cdq_cmd.adm.cdq_id;
	*readfd = cdq_cmd.adm.read_fd;

out:
	close(fd);
	return ret;
}

int do_action_delete(const uint16_t cdq_id)
{
	int fd;
	struct nvme_cdq_cmd cdq_cmd;

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_DELETE;
	cdq_cmd.adm.cdq_id = cdq_id;

	fd = get_bdf_fd(cntl_bdf);
	if (fd < 0)
		return -1;


	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_DELETE");
		return -1;
	}

	return 0;
}

#define NVME_ADMIN_TRACK_SEND			0x3d
#define NVME_CDQ_SEL_LOG_USER_DATA_TRACKSEND	0x0
#define NVME_CDQ_ADM_FLAGS_TR_SEND_START	0x1
#define NVME_CDQ_ADM_FLAGS_TR_SEND_STOP		0x0

int do_action_trsend_cmd(const uint16_t action, const uint16_t cdq_id)
{
	int fd, ret = 0;
	struct nvme_admin_cmd cmd;
	struct nvme_cmd_cdq cdq;

	cdq = (struct nvme_cmd_cdq){
		.opcode = NVME_ADMIN_TRACK_SEND,
		.sel = NVME_CDQ_SEL_LOG_USER_DATA_TRACKSEND,
		.mos = cpu_to_le16(action),
		.cdq_id = cpu_to_le16(cdq_id)
	};

	memcpy(&cmd, &cdq, sizeof(cdq));

	fd = get_bdf_fd(cntl_bdf);
	if (fd < 0)
		return -1;

	if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd)) {
		log_debug("failed sending the track command");
		ret = -1;
	}

	close(fd);
	return ret;
}

int do_action_readfd(const int readfd)
{
	int ret = 0;
	void *buf;
	uint max_retries = 10;
	size_t buf_size;

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	log_debug("File descriptor CDQ: (%d)\n", readfd);

	buf_size = entry_nbyte * entry_nr;
	buf = zmalloc(buf_size);
	if (!buf){
		log_debug("failed in zmalloc");
		return -1;
	}

	hexdump(buf, buf_size);

	for (max_retries = UINT_MAX - max_retries; max_retries != 0; ++max_retries)
	{
		sleep(1);
		log_debug("Reading on %d for %ld\n", readfd, buf_size);
		ret = read(readfd, buf, buf_size);
		if (ret < 0) {
			log_debug("failed on entries read");
			goto free_buf;
		}
		if (ret == 0) {
			log_debug("read returned 0... try again, try number %d\n",
				  UINT_MAX - max_retries);
			continue;
		} else {
			hexdump(buf, buf_size);
			ret = 0;
		}
	}

free_buf:
	free(buf);
	return ret;
}

int main(int argc, char **argv)
{
	pid_t pid;
	int readfd, ret;
	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(cntl_bdf, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");

	if (streq(action, ""))
		opt_usage_exit_fail("missing --action");

	opt_free_table();

	if (strncmp(action, "create", 6) == 0) {
		uint16_t create_cdqid;
		ret = do_action_create(&create_cdqid, &readfd);
		if (ret)
			return ret;

		if (streq(action, "create&read")) {
			pid = fork();

			if (pid < 0) {
				log_debug("failed to fork a read");
				return -1;
			} else if (pid == 0) {
				setsid();
				do_action_readfd(readfd);
				goto out;
			}
		}

		fprintf(stdout, "%d\n", create_cdqid);
		fflush(stdout);
		goto out;
	}

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

	if (streq(action, "tr_send_start")) {
		return do_action_trsend_cmd(NVME_CDQ_ADM_FLAGS_TR_SEND_START, cdqid);
	} else if (streq(action, "delete")) {
		return do_action_delete(cdqid);
	}

out:
	exit(0);
}
