// SPDX-License-Identifier: GPL-2.0-only
/*
 * vDPA bus.
 *
 * Copyright (c) 2020, Red Hat. All rights reserved.
 *     Author: Jason Wang <jasowang@redhat.com>
 *
 */

#include <linux/module.h>
#include <linux/idr.h>
#include <linux/vdpa.h>

static DEFINE_IDA(vdpa_index_ida);

static int vdpa_dev_probe(struct device *d)
{
	struct vdpa_device *vdev = dev_to_vdpa(d);
	struct vdpa_driver *drv = drv_to_vdpa(vdev->dev.driver);
	int ret = 0;

	if (drv && drv->probe)
		ret = drv->probe(vdev);

	return ret;
}

static int vdpa_dev_remove(struct device *d)
{
	struct vdpa_device *vdev = dev_to_vdpa(d);
	struct vdpa_driver *drv = drv_to_vdpa(vdev->dev.driver);

	if (drv && drv->remove)
		drv->remove(vdev);

	return 0;
}

static struct bus_type vdpa_bus = {
	.name  = "vdpa",
	.probe = vdpa_dev_probe,
	.remove = vdpa_dev_remove,
};

/**
 * vdpa_init_device - initilaize a vDPA device
 * This allows driver to some prepartion between after device is
 * initialized but before vdpa_register_device()
 * @vdev: the vdpa device to be initialized
 * @parent: the paretn device
 * @dma_dev: the actual device that is performing DMA
 * @config: the bus operations support by this device
 *
 * Returns an error when parent/config/dma_dev is not set or fail to get
 * ida.
 */
int vdpa_init_device(struct vdpa_device *vdev, struct device *parent,
		     struct device *dma_dev,
		     const struct vdpa_config_ops *config)
{
	int err;

	if (!parent || !dma_dev || !config)
		return -EINVAL;

	err = ida_simple_get(&vdpa_index_ida, 0, 0, GFP_KERNEL);
	if (err < 0)
		return -EFAULT;

	vdev->dev.bus = &vdpa_bus;
	vdev->dev.parent = parent;

	device_initialize(&vdev->dev);

	vdev->index = err;
	vdev->dma_dev = dma_dev;
	vdev->config = config;

	dev_set_name(&vdev->dev, "vdpa%u", vdev->index);

	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_init_device);

/**
 * vdpa_register_device - register a vDPA device
 * Callers must have a succeed call of vdpa_init_device() before.
 * @vdev: the vdpa device to be registered to vDPA bus
 *
 * Returns an error when fail to add to vDPA bus
 */
int vdpa_register_device(struct vdpa_device *vdev)
{
	int err = device_add(&vdev->dev);

	if (err) {
		put_device(&vdev->dev);
		ida_simple_remove(&vdpa_index_ida, vdev->index);
	}

	return err;
}
EXPORT_SYMBOL_GPL(vdpa_register_device);

/**
 * vdpa_unregister_device - unregister a vDPA device
 * @vdev: the vdpa device to be unregisted from vDPA bus
 */
void vdpa_unregister_device(struct vdpa_device *vdev)
{
	int index = vdev->index;

	device_unregister(&vdev->dev);
	ida_simple_remove(&vdpa_index_ida, index);
}
EXPORT_SYMBOL_GPL(vdpa_unregister_device);

/**
 * __vdpa_register_driver - register a vDPA device driver
 * @drv: the vdpa device driver to be registered
 * @owner: module owner of the driver
 *
 * Returns an err when fail to do the registration
 */
int __vdpa_register_driver(struct vdpa_driver *drv, struct module *owner)
{
	drv->driver.bus = &vdpa_bus;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__vdpa_register_driver);

/**
 * vdpa_unregister_driver - unregister a vDPA device driver
 * @drv: the vdpa device driver to be unregistered
 */
void vdpa_unregister_driver(struct vdpa_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(vdpa_unregister_driver);

static int vdpa_init(void)
{
	if (bus_register(&vdpa_bus) != 0)
		panic("virtio bus registration failed");
	return 0;
}

static void __exit vdpa_exit(void)
{
	bus_unregister(&vdpa_bus);
	ida_destroy(&vdpa_index_ida);
}
core_initcall(vdpa_init);
module_exit(vdpa_exit);

MODULE_AUTHOR("Jason Wang <jasowang@redhat.com>");
MODULE_LICENSE("GPL v2");
