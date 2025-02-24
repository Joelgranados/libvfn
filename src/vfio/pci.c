// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#define log_fmt(fmt) "vfio/pci: " fmt

#include <assert.h>
#include <byteswap.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/limits.h>
#include <linux/vfio.h>
#include <linux/pci_regs.h>

#include "vfn/support/atomic.h"
#include "vfn/support/compiler.h"
#include "vfn/support/log.h"
#include "vfn/support/mem.h"
#include "vfn/vfio/container.h"
#include "vfn/vfio/device.h"
#include "vfn/vfio/pci.h"
#include "vfn/pci/util.h"

#include "ccan/array_size/array_size.h"
#include "ccan/minmax/minmax.h"
#include "ccan/str/str.h"

#include "iommu.h"
#include "container.h"

static int pci_set_bus_master(struct vfio_pci_device *pci)
{
	uint16_t pci_cmd;

	if (vfio_pci_read_config(pci, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND) < 0) {
		log_debug("failed to read pci config region\n");
		return -1;
	}

	pci_cmd |= PCI_COMMAND_MASTER;

	if (vfio_pci_write_config(pci, &pci_cmd, sizeof(pci_cmd), PCI_COMMAND) < 0) {
		log_debug("failed to write pci config region\n");
		return -1;
	}

	return 0;
}

static int vfio_pci_init_bar(struct vfio_pci_device *pci, unsigned int idx)
{
	assert(idx < ARRAY_SIZE(pci->bar_region_info));

	pci->bar_region_info[idx] = (struct vfio_region_info) {
		.index = VFIO_PCI_BAR0_REGION_INDEX + idx,
		.argsz = sizeof(struct vfio_region_info),
	};

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_REGION_INFO, &pci->bar_region_info[idx])) {
		log_debug("failed to get bar region info\n");
		return -1;
	}

	return 0;
}

static int vfio_pci_init_irq(struct vfio_pci_device *pci)
{
	int irq_index = VFIO_PCI_MSIX_IRQ_INDEX;

	memset(&pci->dev.irq_info, 0x0, sizeof(pci->dev.irq_info));
	pci->dev.irq_info.argsz = sizeof(pci->dev.irq_info);

	/* prefer msi-x/msi and fall back to intx */
	do {
		if (irq_index < 0) {
			errno = EINVAL;
			log_debug("no supported irq types\n");
			return -1;
		}

		pci->dev.irq_info.index = irq_index--;

		if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_IRQ_INFO, &pci->dev.irq_info)) {
			log_debug("failed to get device irq info\n");
			return -1;
		}
	} while (!pci->dev.irq_info.count);

	log_info("irq_info.count %d\n", pci->dev.irq_info.count);

	return 0;
}

void *vfio_pci_map_bar(struct vfio_pci_device *pci, unsigned int idx, size_t len, uint64_t offset,
		       int prot)
{
	void *mem;

	assert(idx < ARRAY_SIZE(pci->bar_region_info));

	len = min_t(size_t, len, pci->bar_region_info[idx].size - offset);
	offset = pci->bar_region_info[idx].offset + offset;

	mem = mmap(NULL, len, prot, MAP_SHARED, pci->dev.fd, offset);
	if (mem == MAP_FAILED) {
		log_debug("failed to map bar region\n");
		mem = NULL;
	}

	return mem;
}

void vfio_pci_unmap_bar(struct vfio_pci_device *pci, unsigned int idx, void *mem, size_t len,
			uint64_t offset)
{
	assert(idx < ARRAY_SIZE(pci->bar_region_info));

	len = min_t(size_t, len, pci->bar_region_info[idx].size - offset);

	if (munmap(mem, len))
		log_debug("failed to unmap bar region\n");
}

int vfio_pci_open(struct vfio_pci_device *pci, const char *bdf)
{
	__autofree char *group = NULL;
	int gfd;

	pci->bdf = bdf;

	group = pci_get_iommu_group(bdf);
	if (!group) {
		log_error("could not determine iommu group for device %s\n", bdf);
		errno = EINVAL;
		return -1;
	}

	log_info("vfio iommu group is %s\n", group);

	if (!pci->dev.vfio)
		pci->dev.vfio = &vfio_default_container;

	gfd = vfio_get_group_fd(pci->dev.vfio, group);
	if (gfd < 0)
		return -1;

	pci->dev.fd = ioctl(gfd, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (pci->dev.fd < 0) {
		log_debug("failed to get device fd\n");
		return -1;
	}

	pci->dev.device_info.argsz = sizeof(struct vfio_device_info);

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_INFO, &pci->dev.device_info)) {
		log_debug("failed to get device info\n");
		return -1;
	}

	assert(pci->dev.device_info.flags & VFIO_DEVICE_FLAGS_PCI);

	pci->config_region_info = (struct vfio_region_info) {
		.argsz = sizeof(pci->config_region_info),
		.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};

	if (ioctl(pci->dev.fd, VFIO_DEVICE_GET_REGION_INFO, &pci->config_region_info)) {
		log_debug("failed to get config region info\n");
		return -1;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(pci->bar_region_info); i++) {
		if (vfio_pci_init_bar(pci, i))
			return -1;
	}

	if (pci_set_bus_master(pci)) {
		log_debug("failed to set pci bus master\n");
		return -1;
	}

	if (vfio_pci_init_irq(pci)) {
		log_debug("failed to initialize irq\n");
		return -1;
	}

	return 0;
}
