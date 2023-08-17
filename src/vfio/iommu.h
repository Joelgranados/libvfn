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

#define IOVA_MAX_39BITS (1ULL << 39)

struct iova_mapping {
	void *vaddr;
	size_t len;
	uint64_t iova;
};

typedef void (*iova_mapping_iter_fn)(void *opaque, struct iova_mapping *m);

struct iommu_state {
	int nranges;
	struct iova_range *iova_ranges;

	pthread_mutex_t lock;

	uint64_t next;

	void *map;
};

void iommu_init(struct iommu_state *iommu);
void iommu_destroy(struct iommu_state *iommu);

int iommu_add_mapping(struct iommu_state *iommu, void *vaddr, size_t len, uint64_t iova);
void iommu_remove_mapping(struct iommu_state *iommu, void *vaddr);

int iommu_get_iova(struct iommu_state *iommu, size_t len, uint64_t *iova);
bool iommu_vaddr_to_iova(struct iommu_state *iommu, void *vaddr, uint64_t *iova);
struct iova_mapping *iommu_find_mapping(struct iommu_state *iommu, void *vaddr);
void iommu_clear(struct iommu_state *iommu);
void iommu_clear_with(struct iommu_state *iommu, iova_mapping_iter_fn fn, void *opaque);
