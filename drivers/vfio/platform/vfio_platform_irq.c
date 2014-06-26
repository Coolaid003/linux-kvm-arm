/*
 * VFIO platform devices interrupt handling
 *
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
#include <linux/eventfd.h>
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
#include <linux/platform_device.h>
#include <linux/irq.h>

#include "vfio_platform_private.h"

static int vfio_set_trigger(struct vfio_platform_device *vdev,
			    int index, int fd);

int vfio_platform_irq_init(struct vfio_platform_device *vdev)
{
	int cnt = 0, i;

	while (platform_get_irq(vdev->pdev, cnt) > 0)
		cnt++;

	vdev->irq = kzalloc(sizeof(struct vfio_platform_irq) * cnt, GFP_KERNEL);
	if (!vdev->irq)
		return -ENOMEM;

	for (i = 0; i < cnt; i++) {
		int hwirq = platform_get_irq(vdev->pdev, i);

		if (hwirq < 0)
			goto err;

		spin_lock_init(&vdev->irq[i].lock);

		vdev->irq[i].flags = VFIO_IRQ_INFO_EVENTFD
					| VFIO_IRQ_INFO_MASKABLE;

		if (irq_get_trigger_type(hwirq) & IRQ_TYPE_LEVEL_MASK)
			vdev->irq[i].flags |= VFIO_IRQ_INFO_AUTOMASKED;

		vdev->irq[i].count = 1;
		vdev->irq[i].hwirq = hwirq;
		vdev->irq[i].masked = false;
	}

	vdev->num_irqs = cnt;

	return 0;
err:
	kfree(vdev->irq);
	return -EINVAL;
}

void vfio_platform_irq_cleanup(struct vfio_platform_device *vdev)
{
	int i;

	for (i = 0; i < vdev->num_irqs; i++)
		vfio_set_trigger(vdev, i, -1);

	vdev->num_irqs = 0;
	kfree(vdev->irq);
}

static irqreturn_t vfio_irq_handler(int irq, void *dev_id)
{
	struct vfio_platform_irq *irq_ctx = dev_id;
	unsigned long flags;
	int ret = IRQ_NONE;

	spin_lock_irqsave(&irq_ctx->lock, flags);

	if (!irq_ctx->masked) {
		ret = IRQ_HANDLED;

		if (irq_ctx->flags & VFIO_IRQ_INFO_AUTOMASKED) {
			disable_irq_nosync(irq_ctx->hwirq);
			irq_ctx->masked = true;
		}
	}

	spin_unlock_irqrestore(&irq_ctx->lock, flags);

	if (ret == IRQ_HANDLED)
		eventfd_signal(irq_ctx->trigger, 1);

	return ret;
}

static void vfio_platform_unmask(struct vfio_platform_irq *irq_ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&irq_ctx->lock, flags);

	if (irq_ctx->masked) {
		enable_irq(irq_ctx->hwirq);
		irq_ctx->masked = false;
	}

	spin_unlock_irqrestore(&irq_ctx->lock, flags);
}

static void vfio_platform_mask(struct vfio_platform_irq *irq_ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&irq_ctx->lock, flags);

	if (!irq_ctx->masked) {
		disable_irq(irq_ctx->hwirq);
		irq_ctx->masked = true;
	}

	spin_unlock_irqrestore(&irq_ctx->lock, flags);
}

static int vfio_platform_unmask_handler(void *opaque, void *unused)
{
	struct vfio_platform_irq *irq_ctx = opaque;

	vfio_platform_unmask(irq_ctx);

	return 0;
}

static int vfio_platform_mask_handler(void *opaque, void *unused)
{
	struct vfio_platform_irq *irq_ctx = opaque;

	vfio_platform_mask(irq_ctx);

	return 0;
}

static int vfio_set_trigger(struct vfio_platform_device *vdev,
			    int index, int fd)
{
	struct vfio_platform_irq *irq = &vdev->irq[index];
	struct eventfd_ctx *trigger;
	int ret;

	if (irq->trigger) {
		free_irq(irq->hwirq, irq);
		kfree(irq->name);
		eventfd_ctx_put(irq->trigger);
		irq->trigger = NULL;
	}

	if (fd < 0) /* Disable only */
		return 0;

	irq->name = kasprintf(GFP_KERNEL, "vfio-irq[%d](%s)",
						irq->hwirq, vdev->pdev->name);
	if (!irq->name)
		return -ENOMEM;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger)) {
		kfree(irq->name);
		return PTR_ERR(trigger);
	}

	irq->trigger = trigger;

	ret = request_irq(irq->hwirq, vfio_irq_handler, 0, irq->name, irq);
	if (ret) {
		kfree(irq->name);
		eventfd_ctx_put(trigger);
		irq->trigger = NULL;
		return ret;
	}

	return 0;
}

static int vfio_platform_set_irq_trigger(struct vfio_platform_device *vdev,
				     unsigned index, unsigned start,
				     unsigned count, uint32_t flags, void *data)
{
	struct vfio_platform_irq *irq = &vdev->irq[index];
	uint8_t arr;
	int32_t fd;

	switch (flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
	case VFIO_IRQ_SET_DATA_NONE:
		if (count == 0)
			return vfio_set_trigger(vdev, index, -1);

		vfio_irq_handler(irq->hwirq, irq);
		return 0;

	case VFIO_IRQ_SET_DATA_BOOL:
		if (copy_from_user(&arr, data, sizeof(uint8_t)))
			return -EFAULT;

		if (arr == 0x1) {
			vfio_irq_handler(irq->hwirq, irq);
			return 0;
		}

		return -EINVAL;

	case VFIO_IRQ_SET_DATA_EVENTFD:
		if (copy_from_user(&fd, data, sizeof(int32_t)))
			return -EFAULT;

		return vfio_set_trigger(vdev, index, fd);
	}

	return -EFAULT;
}

static int vfio_platform_set_irq_unmask(struct vfio_platform_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	uint8_t arr;
	int32_t fd;

	if (start != 0 || count != 1)
		return -EINVAL;

	switch (flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
	case VFIO_IRQ_SET_DATA_BOOL:
		if (copy_from_user(&arr, data, sizeof(uint8_t)))
			return -EFAULT;

		if (arr != 0x1)
			return -EINVAL;

	case VFIO_IRQ_SET_DATA_NONE:
		vfio_platform_unmask(&vdev->irq[index]);
		return 0;

	case VFIO_IRQ_SET_DATA_EVENTFD:
		if (copy_from_user(&fd, data, sizeof(int32_t)))
			return -EFAULT;

		if (fd >= 0)
			return virqfd_enable((void *) &vdev->irq[index],
					     vfio_platform_unmask_handler,
					     NULL, NULL,
					     &vdev->irq[index].unmask, fd);

		virqfd_disable(&vdev->irq[index].unmask);
		return 0;

	default:
		return -ENOTTY;
	}

	return 0;
}

static int vfio_platform_set_irq_mask(struct vfio_platform_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	uint8_t arr;
	int32_t fd;

	if (start != 0 || count != 1)
		return -EINVAL;

	switch (flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
	case VFIO_IRQ_SET_DATA_BOOL:
		if (copy_from_user(&arr, data, sizeof(uint8_t)))
			return -EFAULT;

		if (arr != 0x1)
			return -EINVAL;

	case VFIO_IRQ_SET_DATA_NONE:
		vfio_platform_mask(&vdev->irq[index]);
		return 0;

	case VFIO_IRQ_SET_DATA_EVENTFD:
		if (copy_from_user(&fd, data, sizeof(int32_t)))
			return -EFAULT;

		if (fd >= 0)
			return virqfd_enable((void *) &vdev->irq[index],
					     vfio_platform_mask_handler,
					     NULL, NULL,
					     &vdev->irq[index].mask, fd);

		virqfd_disable(&vdev->irq[index].mask);
		return 0;

	default:
		return -ENOTTY;
	}

	return 0;
}

int vfio_platform_set_irqs_ioctl(struct vfio_platform_device *vdev,
				 uint32_t flags, unsigned index, unsigned start,
				 unsigned count, void *data)
{
	int (*func)(struct vfio_platform_device *vdev, unsigned index,
		    unsigned start, unsigned count, uint32_t flags,
		    void *data) = NULL;

	switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
	case VFIO_IRQ_SET_ACTION_MASK:
		func = vfio_platform_set_irq_mask;
		break;
	case VFIO_IRQ_SET_ACTION_UNMASK:
		func = vfio_platform_set_irq_unmask;
		break;
	case VFIO_IRQ_SET_ACTION_TRIGGER:
		func = vfio_platform_set_irq_trigger;
		break;
	}

	if (!func)
		return -ENOTTY;

	return func(vdev, index, start, count, flags, data);
}
