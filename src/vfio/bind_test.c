#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

int main()
{
	int iommufd, devfd, err;
	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};
	struct iommu_ioas_alloc alloc_data = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};
	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
		.flags = 0,
	};
	struct iommu_ioas_map map = {
		.size = sizeof(map),
		.flags = IOMMU_IOAS_MAP_READABLE |
			 IOMMU_IOAS_MAP_WRITEABLE |
			 IOMMU_IOAS_MAP_FIXED_IOVA,
		.__reserved = 0,
	};
	char * devpath = "/dev/vfio/devices/vfio0";

	iommufd = open("/dev/iommu", O_RDWR);
	if (iommufd < 0) {
		fprintf(stderr, "Error opening /dev/iommu\n");
		err = -errno;
		goto exit;
	}
	fprintf(stderr, "Opened /dev/iommu with fd %d\n", iommufd);

	devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		fprintf(stderr, "Error opening %s\n", devpath);
		err = -errno;
		goto close_iommu;
	}
	fprintf(stderr, "Opened %s with fd %d\n", devpath, devfd);

	bind.iommufd = iommufd;
	err = ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	if (err) {
		fprintf(stderr, "Error sending the BIND_IOMMUFD ioctl (%d, %d)\n", err, errno);
		err = -errno;
		goto close_dev;
	}
	fprintf(stderr, "device file descriptor (%d) and iommufd file descriptor (%d)"
			" have been bound \n",
		devfd, iommufd);

	err = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	if (err) {
		fprintf(stderr, "Error sending the IOMMU_IOAS_ALLOC  ioctl (%d, %d)\n",
			err, errno);
		err = -errno;
		goto close_dev;
	}

	attach_data.pt_id = alloc_data.out_ioas_id;
	err = ioctl(devfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	if (err) {
		fprintf(stderr, "Error sending the VFIO_DEVICE_ATTACH_IOMMUFD_PT"
				" ioctl (%d, %d)\n", err, errno);
		err = -errno;
		goto close_dev;
	}

	/* We do the memory allocation */
	map.user_va = (uint64_t)mmap(0, 1024*1024, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	map.iova = 0;
	map.length = 1024 * 1024;
	map.ioas_id = alloc_data.out_ioas_id;
	err = ioctl(iommufd, IOMMU_IOAS_MAP, &map);
	if (err) {
		fprintf(stderr, "Error while executing the IOMMU_IOAS_MAP ioctl "
				" (%d, %d)\n", err, errno);
		err = -errno;
		goto close_dev;
	}

close_dev:
	err = close(devfd);
	if (err) {
		fprintf(stderr, "Error closing /dev/nvme0n1\n");
		err = -errno;
	}
close_iommu:
	err = close(iommufd);
	if (err) {
		fprintf(stderr, "Error closing /dev/iommu\n");
		err = -errno;
	}
exit:
	return err;
}
