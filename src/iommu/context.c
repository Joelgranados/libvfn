// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2023 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#define log_fmt(fmt) "iommu/context: " fmt

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include <sys/stat.h>

#include "vfn/support.h"

#include "util/iova_map.h"
#include "context.h"

#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD

static bool __dev_dir_exist;
static bool iommufd_devices_directory_exists(void)
{
	struct stat sb;
	static bool *__dev_dir_exists_ptr = NULL;

	if (__dev_dir_exists_ptr)
		return *__dev_dir_exists_ptr;

	__dev_dir_exists_ptr = &__dev_dir_exist;
	*__dev_dir_exists_ptr = true;
	if (stat("/dev/vfio/devices", &sb) || !S_ISDIR(sb.st_mode)) {
		log_info("Could not find /dev/vfio/devices. "
			 "You need to either unbind the device from current driver "
			 "or kernel was not compiled with CONFIG_VFIO_DEVICE_CDEV=y\n");

		*__dev_dir_exists_ptr = false;
	}

	return *__dev_dir_exists_ptr;
}
#endif

struct iommu_ctx *iommu_get_default_context(void)
{
#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
	if (!iommufd_devices_directory_exists()) {
		log_info("Cannot use iommufd. Will try vfio instead.\n");
		goto fallback;
	}

	return iommufd_get_default_iommu_context();

fallback:
#endif
	return vfio_get_default_iommu_context();
}

struct iommu_ctx *iommu_get_context(const char *name)
{
#ifdef HAVE_VFIO_DEVICE_BIND_IOMMUFD
	if (!iommufd_devices_directory_exists()) {
		log_info("Cannot use iommufd. Will try vfio instead.\n");
		goto fallback;
	}

	return iommufd_get_iommu_context(name);

fallback:
#endif
	return vfio_get_iommu_context(name);
}
