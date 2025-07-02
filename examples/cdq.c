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
#include <sys/wait.h>
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"
#include "linux/nvme_ioctl.h"
#include "stdlib.h"
#include "vfn/support/log.h"

static char *cntl_bdf = "";
static uint test_num[255];
static size_t test_num_count = 0;
static uint cntlids[255];
static size_t cntlids_count = 0;
static uint entry_nbyte = 0;
static uint entry_nr = 0;
static uint verbose = 0;
static uint max_retries = 10;
bool s_usage;

#define MAX_TEST_NUM	2

void hexdump(const void *data, size_t size) {
	const unsigned char *byte = (const unsigned char *)data;
	size_t i, j;

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
int do_action_create(int cntl_fd, uint cntlid, uint16_t *cdqid, int *readfd)
{
	int ret = 0;
	struct nvme_cdq_cmd cdq_cmd;

	if (verbose > 0)
		fprintf(stderr, "child cntl : %d\n", cntlid);

	cdq_cmd.entry_nbyte = entry_nbyte;
	cdq_cmd.entry_nr = entry_nr;
	cdq_cmd.cqs = cntlid;
	cdq_cmd.mos = NVME_CDQ_MOS_CREATE_QT_UDMQ;

	// Guess that its in the last bit
	cdq_cmd.cdqp_offset = entry_nbyte - 1;
	cdq_cmd.cdqp_mask = 0x1;

	if (ioctl(cntl_fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_CREATE");
		ret = -1;
		goto out;
	}

	*cdqid = cdq_cmd.cdq_id;
	*readfd = cdq_cmd.read_fd;

out:
	return ret;
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

int do_action_readfd(const int readfd, uint rep_count)
{
	int ret = 0;
	void *buf;
	size_t buf_size, read_accum = 0;

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

	for (rep_count = UINT_MAX - rep_count; rep_count != 0; ++rep_count)
	{
		sleep(1);
		log_debug("Reading on %d for %ld\n", readfd, buf_size);
		ret = read(readfd, buf, buf_size);
		if (ret < 0) {
			log_debug("failed on entries read");
			goto free_buf;
		}

		if (ret > 0) {
			read_accum += ret;
			hexdump(buf, buf_size);
		}

		log_debug("read: ret %d, accum %ld  (%d)\n", ret, read_accum,  - rep_count);
	}

free_buf:
	free(buf);
	return ret;
}

void t0(int cntl_fd)
{
	pid_t pid;
	int ret, cdq_fd;
	uint16_t cdq_id;

	log_debug("Executing test 0\n");

	ret = do_action_create(cntl_fd, cntlids[0], &cdq_id, &cdq_fd);
	if (ret) {
		log_error("Failed to create cdq on %d\n", cntl_fd);
		return;
	}

	pid = fork();

	if (pid < 0) {
		log_error("failed to fork a read");
		return;
	} else if (pid == 0) {
		setsid();
		do_action_readfd(cdq_fd, max_retries);
		exit(0);
	}

	ret = do_action_trsend_cmd(NVME_CDQ_ADM_FLAGS_TR_SEND_START, cdq_id);
	if (ret) {
		log_error("do_action_trsend_cmd exited erroneously\n");
		return;
	}

	wait(&ret);

	if (!WIFEXITED(ret))
		log_error("read fd child exited erroneously\n");

	if (close(cdq_fd))
		log_error("Could not close exit the cdq fd properly\n");
}

void t1(int cntl_fd)
{
	pid_t pid;
	uint16_t cdq_id1, cdq_id2;
	int cdq_fd1, cdq_fd2, ret;

	log_debug("Executing test 1\n");
	if (cntlids_count < 2) {
		log_error("Too few cntlids for t1\n");
		return;
	}

	ret = do_action_create(cntl_fd, cntlids[0], &cdq_id1, &cdq_fd1);
	if (ret) {
		log_error("Failed to create cdq1 on %d\n", cntl_fd);
		return;
	}

	ret = do_action_create(cntl_fd, cntlids[1], &cdq_id2, &cdq_fd2);
	if (ret) {
		log_error("Failed to create cdq2 on %d\n", cntl_fd);
		return;
	}

	if (close(cdq_fd1)) {
		log_error("close cdq on cdq_id: %d exited erroneously\n", cdq_id1);
		return;
	}

	pid = fork();

	if (pid < 0) {
		log_error("failed to fork a read");
		return;
	} else if (pid == 0) {
		setsid();
		do_action_readfd(cdq_fd2, max_retries);
		exit(0);
	}

	ret = do_action_trsend_cmd(NVME_CDQ_ADM_FLAGS_TR_SEND_START, cdq_id2);
	if (ret) {
		log_debug("do_action_trsend_cmd exited erroneously\n");
		return;
	}

	wait(&ret);

	if (!WIFEXITED(ret))
		log_error("read fd child exited erroneously\n");

	if (close(cdq_fd2))
		log_error("close cdq on cdq_id: %d exited erroneously\n", cdq_id2);

}

void (*test_funcs[MAX_TEST_NUM])(int)
	= {t0, t1};

static char *collect_test_num(const char *optarg, __attribute__((__unused__)) void *unused)
{
	uint t;
	char *ret = opt_set_uintval(optarg, &t);
	if (ret != NULL)
		return ret;

	test_num[test_num_count++] = t;
	return NULL;
}

static char *collect_cntlid(const char *optarg, __attribute__((__unused__)) void *unused)
{
	uint t;
	char *ret = opt_set_uintval(optarg, &t);
	if (ret != NULL)
		return ret;

	cntlids[cntlids_count++] = t;
	return NULL;
}

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&entry_nr, "Number of entries in CDQ"),
	OPT_WITH_ARG("--cntl-bdf",
			opt_set_charp, opt_show_charp,
			&cntl_bdf, "Controller Bus:Device:Func Id"),
	OPT_WITH_ARG("--max-retries",
			opt_set_uintval, opt_show_uintval,
			&max_retries, "Controller Bus:Device:Func Id"),
	OPT_WITH_ARG("--verbose",
			opt_set_uintval, opt_show_uintval,
			&verbose, "Verbosity value"),
	OPT_ENDTABLE,
};

int main(int argc, char **argv)
{
	int cntl_fd;
	opt_register_table(opts, NULL);
	opt_register_arg("--test-num", collect_test_num, NULL, NULL, "");
	opt_register_arg("--child-cntl", collect_cntlid, NULL, NULL, "");
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(cntl_bdf, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");
	if (test_num_count == 0)
		opt_usage_exit_fail("must pass --test-num [0,%d) at least once", MAX_TEST_NUM);
	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");
	if (cntlids_count == 0)
		opt_usage_exit_fail("must pass at least one --child-cntl");

	opt_free_table();

	cntl_fd = get_bdf_fd(cntl_bdf);
	if (cntl_fd < 0) {
		errno = -EPERM;
		log_fatal("Error getting fd for %s\n", cntl_bdf);
	}

	for(size_t i = 0; i < test_num_count; ++i) {
		if (test_num[i] >= MAX_TEST_NUM) {
			errno = -EINVAL;
			log_fatal("Unknown test number: %d", test_num[i]);
		}

		test_funcs[test_num[i]](cntl_fd);
	}

	exit(0);
}
