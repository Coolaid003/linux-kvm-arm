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
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/irq.h>

#include "vfio_platform_private.h"

#define DRIVER_VERSION  "0.7"
#define DRIVER_AUTHOR   "Antonios Motakis <a.motakis@virtualopensystems.com>"
#define DRIVER_DESC     "VFIO for platform devices - User Level meta-driver"

/* probing devices from the linux platform bus */

static struct resource *get_platform_resource(struct vfio_platform_device *vdev,
						int i)
{
	struct platform_device *pdev = (struct platform_device *) vdev->opaque;

	return platform_get_resource(pdev, IORESOURCE_MEM, i);
}

static int get_platform_irq(struct vfio_platform_device *vdev, int i)
{
	struct platform_device *pdev = (struct platform_device *) vdev->opaque;

	return platform_get_irq(pdev, i);
}


static int vfio_platform_probe(struct platform_device *pdev)
{
	struct vfio_platform_device *vdev;
	int ret;

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	vdev->opaque = (void *) pdev;
	vdev->name = pdev->name;
	vdev->flags = VFIO_DEVICE_FLAGS_PLATFORM;
	vdev->get_resource = get_platform_resource;
	vdev->get_irq = get_platform_irq;

	ret = vfio_platform_probe_common(vdev, &pdev->dev);
	if (ret)
		kfree(vdev);

	return ret;
}

static int vfio_platform_remove(struct platform_device *pdev)
{
	return vfio_platform_remove_common(&pdev->dev);
}

static struct platform_driver vfio_platform_driver = {
	.probe		= vfio_platform_probe,
	.remove		= vfio_platform_remove,
	.driver	= {
		.name	= "vfio-platform",
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(vfio_platform_driver);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
