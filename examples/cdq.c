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
	OPT_WITH_ARG("-A|--action",
			opt_set_charp, opt_show_charp,
			&action, "Action to perform: create, tr_send_start, delete, readfd"),
	OPT_WITH_ARG("--entry-nbyte",
			opt_set_uintval, opt_show_uintval,
			&entry_nbyte, "Size in bytes of the CDQ entries"),
	OPT_WITH_ARG("--entry-nr",
			opt_set_uintval, opt_show_uintval,
			&entry_nr, "Number of entries in CDQ"),
	OPT_WITH_ARG("--verbose",
			opt_set_uintval, opt_show_uintval,
			&verbose, "Verbosity value"),
	OPT_WITH_ARG("--cntl-bdf",
			opt_set_charp, opt_show_charp,
			&cntl_bdf, "Controller Bus:Device:Func Id"),
	OPT_ENDTABLE,
};

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
int do_action_create(void)
{
	int fd, ret = 0;
	struct nvme_cdq_cmd cdq_cmd;

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

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
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_CDQ_ADM_FLAGS_CREATE");
		ret = -1;
		goto out;
	}

	fprintf(stdout, "%d\n", cdq_cmd.adm.cdq_id);

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

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

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
	int ret = 0, fd, cdq_fd;
	void *buf;
	uint max_retries = 50;
	size_t buf_size;
	struct nvme_cdq_cmd cdq_cmd = {};

	if (cdqid == UINT_MAX)
		opt_usage_exit_fail("missing controller data queue id");

	if (entry_nbyte == 0 || entry_nr == 0)
		opt_usage_exit_fail("--entry-nbyte and --entry-nr need to be >0");

	fd = get_bdf_fd(cntl_bdf);
	if (fd < 0)
		return -1;

	cdq_cmd.flags = NVME_CDQ_ADM_FLAGS_READFD;
	cdq_cmd.readfd.cdqid = (uint16_t)cdqid;
	if (ioctl(fd, NVME_IOCTL_ADMIN_CDQ, &cdq_cmd)) {
		log_debug("failed on NVME_IOCTL_ADM_FLASG_READFD");
		ret = -1;
		goto close_fd;
	}
	cdq_fd = cdq_cmd.readfd.read_fd;
	log_debug("File descriptor CDQ: (%d)\n", cdq_fd);

	buf_size = entry_nbyte * entry_nr;
	buf = zmalloc(buf_size);
	if (!buf){
		log_debug("failed in zmalloc");
		ret = -1;
		goto close_fd;
	}

	hexdump(buf, buf_size);

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
			ret = 0;
		}
	}

	free(buf);
	close(cdq_fd);

close_fd:
	close(fd);

	return ret;
}

int main(int argc, char **argv)
{

	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(cntl_bdf, ""))
		opt_usage_exit_fail("missing --parent-cntl (parent controller device path)");

	if (streq(action, ""))
		opt_usage_exit_fail("missing --action");

	opt_free_table();

	if(streq(action, "create")) {
		return do_action_create();
	} else if (streq(action, "delete")) {
		return do_action_delete(cdqid);
	} else if (streq(action, "tr_send_start")) {
		return do_action_trsend_cmd(NVME_CDQ_ADM_FLAGS_TR_SEND_START, cdqid);
	} else if (streq(action, "readfd")) {
		return do_action_readfd();
	} else
		opt_usage_exit_fail("incorrect action string %s", action);

	return 0;
}
