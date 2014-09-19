/*
 * Copyright (C) 2013 - Virtual Open Systems
 * Author: Antonios Motakis <a.motakis@virtualopensystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/io.h>

#include "vfio_platform_private.h"

static int vfio_platform_regions_init(struct vfio_platform_device *vdev)
{
	int cnt = 0, i;

	while (vdev->get_resource(vdev, cnt))
		cnt++;

	vdev->regions = kcalloc(cnt, sizeof(struct vfio_platform_region),
				GFP_KERNEL);
	if (!vdev->regions)
		return -ENOMEM;

	for (i = 0; i < cnt;  i++) {
		struct resource *res =
			vdev->get_resource(vdev, i);

		if (!res)
			goto err;

		vdev->regions[i].addr = res->start;
		vdev->regions[i].size = resource_size(res);
		vdev->regions[i].flags = VFIO_REGION_INFO_FLAG_READ
					| VFIO_REGION_INFO_FLAG_WRITE;
		/* Only regions addressed with PAGE granularity may be MMAPed
		 * securely. */
		if (!(vdev->regions[i].addr & ~PAGE_MASK)
				&& !(vdev->regions[i].size & ~PAGE_MASK))
			vdev->regions[i].flags |= VFIO_REGION_INFO_FLAG_MMAP;
	}

	vdev->num_regions = cnt;

	return 0;
err:
	kfree(vdev->regions);
	return -EINVAL;
}

static void vfio_platform_regions_cleanup(struct vfio_platform_device *vdev)
{
	int i;

	for (i = 0; i < vdev->num_regions; i++)
		iounmap(vdev->regions[i].ioaddr);

	vdev->num_regions = 0;
	kfree(vdev->regions);
}

static void vfio_platform_release(void *device_data)
{
	struct vfio_platform_device *vdev = device_data;

	if (atomic_dec_and_test(&vdev->refcnt)) {
		vfio_platform_regions_cleanup(vdev);
		vfio_platform_irq_cleanup(vdev);
	}

	module_put(THIS_MODULE);
}

static int vfio_platform_open(void *device_data)
{
	struct vfio_platform_device *vdev = device_data;
	int ret;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	if (atomic_inc_return(&vdev->refcnt) == 1) {
		ret = vfio_platform_regions_init(vdev);
		if (ret)
			goto err_reg;

		ret = vfio_platform_irq_init(vdev);
		if (ret)
			goto err_irq;
	}

	return 0;

err_irq:
	vfio_platform_regions_cleanup(vdev);
err_reg:
	module_put(THIS_MODULE);
	return ret;
}

static long vfio_platform_ioctl(void *device_data,
			   unsigned int cmd, unsigned long arg)
{
	struct vfio_platform_device *vdev = device_data;
	unsigned long minsz;

	if (cmd == VFIO_DEVICE_GET_INFO) {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = vdev->flags;
		info.num_regions = vdev->num_regions;
		info.num_irqs = vdev->num_irqs;

		return copy_to_user((void __user *)arg, &info, minsz);

	} else if (cmd == VFIO_DEVICE_GET_REGION_INFO) {
		struct vfio_region_info info;

		minsz = offsetofend(struct vfio_region_info, offset);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		if (info.index >= vdev->num_regions)
			return -EINVAL;

		/* map offset to the physical address  */
		info.offset = VFIO_PLATFORM_INDEX_TO_OFFSET(info.index);
		info.size = vdev->regions[info.index].size;
		info.flags = vdev->regions[info.index].flags;

		return copy_to_user((void __user *)arg, &info, minsz);

	} else if (cmd == VFIO_DEVICE_GET_IRQ_INFO) {
		struct vfio_irq_info info;

		minsz = offsetofend(struct vfio_irq_info, count);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		if (info.index >= vdev->num_irqs)
			return -EINVAL;

		info.flags = vdev->irqs[info.index].flags;
		info.count = vdev->irqs[info.index].count;

		return copy_to_user((void __user *)arg, &info, minsz);

	} else if (cmd == VFIO_DEVICE_SET_IRQS) {
		struct vfio_irq_set hdr;
		int ret = 0;

		minsz = offsetofend(struct vfio_irq_set, count);

		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;

		if (hdr.argsz < minsz)
			return -EINVAL;

		if (hdr.index >= vdev->num_irqs)
			return -EINVAL;

		if (hdr.start != 0 || hdr.count > 1)
			return -EINVAL;

		if (hdr.count == 0 &&
			(!(hdr.flags & VFIO_IRQ_SET_DATA_NONE) ||
			 !(hdr.flags & VFIO_IRQ_SET_ACTION_TRIGGER)))
			return -EINVAL;

		if (hdr.flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK |
				  VFIO_IRQ_SET_ACTION_TYPE_MASK))
			return -EINVAL;

		mutex_lock(&vdev->igate);

		ret = vfio_platform_set_irqs_ioctl(vdev, hdr.flags, hdr.index,
						   hdr.start, hdr.count,
						   (void *)arg+minsz);
		mutex_unlock(&vdev->igate);

		return ret;

	} else if (cmd == VFIO_DEVICE_RESET)
		return -EINVAL;

	return -ENOTTY;
}

static ssize_t vfio_platform_read(void *device_data, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct vfio_platform_device *vdev = device_data;
	unsigned int index = VFIO_PLATFORM_OFFSET_TO_INDEX(*ppos);
	loff_t off = *ppos & VFIO_PLATFORM_OFFSET_MASK;
	unsigned int done = 0;

	if (index >= vdev->num_regions)
		return -EINVAL;

	if (!vdev->regions[index].ioaddr) {
		vdev->regions[index].ioaddr =
			ioremap_nocache(vdev->regions[index].addr,
					vdev->regions[index].size);

		if (!vdev->regions[index].ioaddr)
			return -ENOMEM;
	}

	while (count) {
		size_t filled;

		if (count >= 4 && !(off % 4)) {
			u32 val;

			val = ioread32(vdev->regions[index].ioaddr + off);
			if (copy_to_user(buf, &val, 4))
				goto err;

			filled = 4;
		} else if (count >= 2 && !(off % 2)) {
			u16 val;

			val = ioread16(vdev->regions[index].ioaddr + off);
			if (copy_to_user(buf, &val, 2))
				goto err;

			filled = 2;
		} else {
			u8 val;

			val = ioread8(vdev->regions[index].ioaddr + off);
			if (copy_to_user(buf, &val, 1))
				goto err;

			filled = 1;
		}


		count -= filled;
		done += filled;
		off += filled;
		buf += filled;
	}

	return done;
err:
	return -EFAULT;
}

static ssize_t vfio_platform_write(void *device_data, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct vfio_platform_device *vdev = device_data;
	unsigned int index = VFIO_PLATFORM_OFFSET_TO_INDEX(*ppos);
	loff_t off = *ppos & VFIO_PLATFORM_OFFSET_MASK;
	unsigned int done = 0;

	if (index >= vdev->num_regions)
		return -EINVAL;

	if (!vdev->regions[index].ioaddr) {
		vdev->regions[index].ioaddr =
			ioremap_nocache(vdev->regions[index].addr,
					vdev->regions[index].size);

		if (!vdev->regions[index].ioaddr)
			return -ENOMEM;
	}

	while (count) {
		size_t filled;

		if (count >= 4 && !(off % 4)) {
			u32 val;

			if (copy_from_user(&val, buf, 4))
				goto err;
			iowrite32(val, vdev->regions[index].ioaddr + off);

			filled = 4;
		} else if (count >= 2 && !(off % 2)) {
			u16 val;

			if (copy_from_user(&val, buf, 2))
				goto err;
			iowrite16(val, vdev->regions[index].ioaddr + off);

			filled = 2;
		} else {
			u8 val;

			if (copy_from_user(&val, buf, 1))
				goto err;
			iowrite8(val, vdev->regions[index].ioaddr + off);

			filled = 1;
		}

		count -= filled;
		done += filled;
		off += filled;
		buf += filled;
	}

	return done;
err:
	return -EFAULT;
}

static int vfio_platform_mmap(void *device_data, struct vm_area_struct *vma)
{
	struct vfio_platform_device *vdev = device_data;
	unsigned int index;
	u64 req_len, pgoff, req_start;
	struct vfio_platform_region regions;

	index = vma->vm_pgoff >> (VFIO_PLATFORM_OFFSET_SHIFT - PAGE_SHIFT);

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;
	if (index >= vdev->num_regions)
		return -EINVAL;
	if (vma->vm_start & ~PAGE_MASK)
		return -EINVAL;
	if (vma->vm_end & ~PAGE_MASK)
		return -EINVAL;

	regions = vdev->regions[index];

	req_len = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PLATFORM_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	if (regions.size < PAGE_SIZE || req_start + req_len > regions.size)
		return -EINVAL;

	vma->vm_private_data = vdev;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = (regions.addr >> PAGE_SHIFT) + pgoff;

	return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			       req_len, vma->vm_page_prot);
}

static const struct vfio_device_ops vfio_platform_ops = {
	.name		= "vfio-platform",
	.open		= vfio_platform_open,
	.release	= vfio_platform_release,
	.ioctl		= vfio_platform_ioctl,
	.read		= vfio_platform_read,
	.write		= vfio_platform_write,
	.mmap		= vfio_platform_mmap,
};

int vfio_platform_probe_common(struct vfio_platform_device *vdev,
			       struct device *dev)
{
	struct iommu_group *group;
	int ret;

	if (!vdev)
		return -EINVAL;

	group = iommu_group_get(dev);
	if (!group) {
		pr_err("VFIO: No IOMMU group for device %s\n", vdev->name);
		return -EINVAL;
	}

	ret = vfio_add_group_dev(dev, &vfio_platform_ops, vdev);
	if (ret) {
		iommu_group_put(group);
		return ret;
	}

	mutex_init(&vdev->igate);

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_platform_probe_common);

int vfio_platform_remove_common(struct device *dev)
{
	struct vfio_platform_device *vdev;

	vdev = vfio_del_group_dev(dev);
	if (!vdev)
		return -EINVAL;

	iommu_group_put(dev->iommu_group);
	kfree(vdev);

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_platform_remove_common);
