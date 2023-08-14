/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifndef LIBVFN_VFIO_CONTAINER_H
#define LIBVFN_VFIO_CONTAINER_H

/**
 * vfio_new - create a new vfio container
 *
 * Create a new VFIO container.
 *
 * Return: Container handle or ``NULL`` on error.
 */
struct vfio_container *vfio_new(void);

#ifndef VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE
struct vfio_iova_range {
	__u64	start;
	__u64	end;
};
#endif

int vfio_get_iova_ranges(struct vfio_container *vfio, struct vfio_iova_range **ranges);

#endif /* LIBVFN_VFIO_CONTAINER_H */
