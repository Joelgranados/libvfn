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
#include <sys/ioctl.h>
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"
#include "linux/nvme_ioctl.h"

static char *parent_cntl = "";
static char *action = "";
static unsigned int cntlid = 0;
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
	OPT_WITH_ARG("-P|--parent-cntl",
			opt_set_charp, opt_show_charp,
			&parent_cntl, "Parent controller device path"),
	OPT_WITH_ARG("-A|--action",
			opt_set_charp, opt_show_charp,
			&action, "Action to perform: alloc, tr_send_start, kthread, readfd"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&entry_nr, "Number of entries in CDQ"),
	OPT_WITH_ARG("--verbose",
			opt_set_uintval, opt_show_uintval,
			&verbose, "Verbosity value"),
	OPT_ENDTABLE,
};

int do_action_alloc(void)
{
	int fd;
	struct nvme_cdq_cmd cdq_cmd;

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	if (verbose > 0) {
		fprintf(stderr, "child cntl : %d\n", cntlid);
		fprintf(stderr, "parent path : %s\n", parent_cntl);
	}

	fd = open(parent_cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_ALLOC;
	cdq_cmd.alloc.entry_nbyte = entry_nbyte;
	cdq_cmd.alloc.entry_nr = entry_nr;
	cdq_cmd.alloc.cntlid = cntlid;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_ALLOC");
		return -1;
	}

	fprintf(stdout, "%d\n", cdq_cmd.alloc.cdqid);
	return 0;
}

int do_action_trsend(const __u8 action)
{

	int fd;
	struct nvme_cdq_cmd cdq_cmd = {};

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

	fd = open(parent_cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_TR_SEND;
	cdq_cmd.tr_send.action = action;
	cdq_cmd.tr_send.cdqid = (uint16_t)cdqid;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_TR_SEND");
		return -1;
	}

	return 0;
}

int do_action_kthread(void)
{
	int fd;
	struct nvme_cdq_cmd cdq_cmd = {};

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

	fd = open(parent_cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_KTHREAD;
	cdq_cmd.kthread.cdqid = (uint16_t)cdqid;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_KTHREAD");
		return -1;
	}

	return 0;
}

void hexdump(const void *data, size_t size) {
    const unsigned char *byte = (const unsigned char *)data;
    size_t i, j;

    for (i = 0; i < size; i += 16) {
        printf("%08zx  ", i);  // Offset

        // Hex bytes
        for (j = 0; j < 16; ++j) {
            if (i + j < size)
                printf("%02x ", byte[i + j]);
            else
                printf("   ");
        }

        printf(" ");

        // ASCII chars
        for (j = 0; j < 16; ++j) {
            if (i + j < size) {
                unsigned char c = byte[i + j];
                printf("%c", isprint(c) ? c : '.');
            }
        }

        printf("\n");
    }
}

int do_action_readfd(void)
{
	int ret, fd, cdq_fd;
	void *buf;
	uint max_retries = 50;
	size_t buf_size;
	struct nvme_cdq_cmd cdq_cmd = {};

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	fd = open(parent_cntl, O_RDWR);
	if (fd < 0) {
		log_debug("failed to open parent controller device path: %s\n", strerror(errno));
		return -1;
	}

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_READFD;
	cdq_cmd.readfd.cdqid = (uint16_t)cdqid;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_IOCTL_ADM_FLASG_READFD");
		return -1;
	}

	cdq_fd = cdq_cmd.readfd.read_fd;
	log_debug("File descriptor CDQ: (%d)\n", cdq_fd);

	buf_size = entry_nbyte * entry_nr;
	buf = malloc(buf_size);
	hexdump(buf, buf_size);
	if (!buf){
		log_debug("failed in malloc");
		close(cdq_fd);
	}

	for (max_retries = UINT_MAX - max_retries; max_retries != 0; ++max_retries)
	{
		sleep(1);
		log_debug("Reading on %d for %ld\n", cdq_fd, buf_size);
		ret = read(cdq_fd, buf, buf_size);
		if (ret < 0) {
			log_debug("failed on entries read");
			return -1;
		}
		if (ret == 0) {
			log_debug("read returned 0... try again, try number %d\n", UINT_MAX - max_retries);
			continue;
		} else {
			hexdump(buf, buf_size);
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(parent_cntl, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");

	if (streq(action, ""))
		opt_usage_exit_fail("missing --action");

	opt_free_table();

	if(streq(action, "alloc")) {
		return do_action_alloc();
	} else if (streq(action, "tr_send_start")) {
		return do_action_trsend(NVME_CDQ_ADM_FLAGS_TR_SEND_START);
	} else if (streq(action, "kthread")) {
		return do_action_kthread();
	} else if (streq(action, "readfd")) {
		return do_action_readfd();
	} else
		opt_usage_exit_fail("incorrect action string %s", action);

	return 0;
}
