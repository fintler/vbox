/** @file $Id$
 *
 * VirtualBox Additions Linux kernel video driver
 */

/*
 * Copyright (C) 2011-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 * --------------------------------------------------------------------
 *
 * This code is based on
 * glint_drv.c
 * with the following copyright and permission notice:
 *
 * Copyright 2010 Matt Turner.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Matt Turner
 */

#include <linux/version.h>
#include <linux/module.h>
#include "version-generated.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)

# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
#  ifdef RHEL_RELEASE_CODE
#   if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(6,1)
#    define DRM_RHEL61
#   endif
#  endif
# endif

#include "vboxvideo_drv.h"

#ifndef __devinit
#define __devinit
#define __devinitdata
#endif

static struct drm_driver driver;

static struct pci_device_id pciidlist[] = {
        vboxvideo_PCI_IDS
};

MODULE_DEVICE_TABLE(pci, pciidlist);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
# define drm_get_dev drm_get_pci_dev
#endif

static int __devinit
vboxvideo_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    return drm_get_dev(pdev, ent, &driver);
}

static void
vboxvideo_pci_remove(struct pci_dev *pdev)
{
    struct drm_device *dev = pci_get_drvdata(pdev);

    drm_put_dev(dev);
}

#if defined(DRM_UNLOCKED) || LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
# define IOCTL_MEMBER unlocked_ioctl
#else
# define IOCTL_MEMBER ioctl
#endif

#define DRIVER_FOPS \
    { \
        .owner = THIS_MODULE, \
        .open = drm_open, \
        .release = drm_release, \
        /* This was changed with Linux 2.6.33 but Fedora backported this \
         * change to their 2.6.32 kernel. */ \
        .IOCTL_MEMBER = drm_ioctl, \
        .mmap = drm_mmap, \
        .poll = drm_poll, \
        .fasync = drm_fasync, \
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0) || defined(DRM_RHEL63) || defined(DRM_DEBIAN_34ON32)
/* since linux-3.3.0-rc1 drm_driver::fops is pointer */
static struct file_operations driver_fops = DRIVER_FOPS;
#endif

#define PCI_DRIVER \
    { \
        .name = DRIVER_NAME, \
        .id_table = pciidlist, \
        .probe = vboxvideo_pci_probe, \
        .remove = vboxvideo_pci_remove, \
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)
static struct pci_driver pci_driver = PCI_DRIVER;
#endif

static struct drm_driver driver =
{
    .driver_features = DRIVER_MODESET,
    .load = vboxvideo_driver_load,
    .unload = vboxvideo_driver_unload,
    /* As of Linux 2.6.37, always the internal functions are used. */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 37) && !defined(DRM_RHEL61)
    .reclaim_buffers = drm_core_reclaim_buffers,
    .get_map_ofs = drm_core_get_map_ofs,
    .get_reg_ofs = drm_core_get_reg_ofs,
#endif
    .ioctls = vboxvideo_ioctls,
# if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0) && !defined(DRM_RHEL63) && !defined(DRM_DEBIAN_34ON32)
    .fops = DRIVER_FOPS,
#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0) || defined(DRM_RHEL63) || defined(DRM_DEBIAN_34ON32) */
    .fops = &driver_fops,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 39)
    .pci_driver = PCI_DRIVER,
#endif
    .name = DRIVER_NAME,
    .desc = DRIVER_DESC,
    .date = DRIVER_DATE,
    .major = DRIVER_MAJOR,
    .minor = DRIVER_MINOR,
    .patchlevel = DRIVER_PATCHLEVEL,
};

static int __init vboxvideo_init(void)
{
     printk(KERN_INFO "vboxvideo initializing\n");
    driver.num_ioctls = vboxvideo_max_ioctl;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 39)
    return drm_init(&driver);
#else
    return drm_pci_init(&driver, &pci_driver);
#endif
}

static void __exit vboxvideo_exit(void)
{
     printk(KERN_INFO "vboxvideo exiting\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 39)
    drm_exit(&driver);
#else
    drm_pci_exit(&driver, &pci_driver);
#endif
}

module_init(vboxvideo_init);
module_exit(vboxvideo_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27) */

#ifdef MODULE_VERSION
MODULE_VERSION(VBOX_VERSION_STRING);
#endif
MODULE_LICENSE("GPL and additional rights");
