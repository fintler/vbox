/** @file $Id$
 *
 * VirtualBox Additions Linux kernel video driver, KMS support
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
 * glint_crtc.c
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

#include <VBox/VBoxVideoGuest.h>

#include "vboxvideo_drv.h"

#include "drm/drm_crtc_helper.h"

/** Set a graphics mode.  Poke any required values into registers, do an HGSMI
 * mode set and tell the host we support advanced graphics functions.
 */
static void vboxvideo_do_modeset(struct drm_crtc *crtc)
{
    struct vboxvideo_crtc   *vboxvideo_crtc = to_vboxvideo_crtc(crtc);
    struct vboxvideo_device *gdev = crtc->dev->dev_private;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
    int pitch = crtc->fb.pitch;
#else
    int pitch = crtc->fb.pitches[0];
#endif

    if (vboxvideo_crtc->crtc_id == 0)
        VBoxVideoSetModeRegisters(vboxvideo_crtc->last_width,
                                  vboxvideo_crtc->last_height,
                                  pitch,
                                  crtc->fb.bits_per_pixel, 0,
                                  vboxvideo_crtc->last_x,
                                  vboxvideo_crtc->last_y);
    if (gdev->fHaveHGSMI)
    {
        uint16_t fFlags = VBVA_SCREEN_F_ACTIVE;
        fFlags |= (vboxvideo_crtc->enabled ? 0 : VBVA_SCREEN_F_DISABLED);
        VBoxHGSMIProcessDisplayInfo(&gdev->Ctx, vboxvideo_crtc->crtc_id,
                                    vboxvideo_crtc->last_x,
                                    vboxvideo_crtc->last_y,
                                        vboxvideo_crtc->last_x
                                      * crtc->fb.bits_per_pixel
                                    + vboxvideo_crtc->last_y * pitch,
                                    pitch,
                                    vboxvideo_crtc->last_width,
                                    vboxvideo_crtc->last_height,
                                    crtc->fb.bits_per_pixel, fFlags);
    }
}


static void vboxvideo_crtc_dpms(struct drm_crtc *crtc, int mode)
{
    struct vboxvideo_crtc *vboxvideo_crtc = to_vboxvideo_crtc(crtc);

    if (mode == vboxvideo_crtc->last_dpms) /* Don't do unnecesary mode changes. */
        return;

    vboxvideo_crtc->last_dpms = mode;

    switch (mode) {
    case DRM_MODE_DPMS_STANDBY:
    case DRM_MODE_DPMS_SUSPEND:
    case DRM_MODE_DPMS_OFF:
        vboxvideo_crtc->enabled = false;
        break;
    case DRM_MODE_DPMS_ON:
        vboxvideo_crtc->enabled = true;
        break;
    }
}

static bool vboxvideo_crtc_mode_fixup(struct drm_crtc *crtc,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
                                     const struct drm_display_mode *mode,
#else
                                     struct drm_display_mode *mode,
#endif
                                     struct drm_display_mode *adjusted_mode)
{
    return true;
}

static int vboxvideo_crtc_mode_set(struct drm_crtc *crtc,
                                   struct drm_display_mode *mode,
                                   struct drm_display_mode *adjusted_mode,
                                   int x, int y, struct drm_framebuffer *old_fb)
{
    /*
    struct vboxvideo_crtc *vboxvideo_crtc = to_vboxvideo_crtc(crtc);

    vboxvideo_crtc_set_base(crtc, x, y, old_fb);
    vboxvideo_set_crtc_timing(crtc, adjusted_mode);
    vboxvideo_set_pll(crtc, adjusted_mode);
    */
    return 0;
}

static void vboxvideo_crtc_prepare(struct drm_crtc *crtc)
{
    struct drm_device *dev = crtc->dev;
    struct drm_crtc *crtci;

    /*
    list_for_each_entry(crtci, &dev->mode_config.crtc_list, head)
        vboxvideo_crtc_dpms(crtci, DRM_MODE_DPMS_OFF);
    */
}

static void vboxvideo_crtc_commit(struct drm_crtc *crtc)
{
    struct drm_device *dev = crtc->dev;
    struct drm_crtc *crtci;

    /*
    list_for_each_entry(crtci, &dev->mode_config.crtc_list, head) {
        if (crtci->enabled)
            vboxvideo_crtc_dpms(crtci, DRM_MODE_DPMS_ON);
    }
    */
}

static void vboxvideo_crtc_load_lut(struct drm_crtc *crtc)
{
    /* Dummy */
}

static void vboxvideo_crtc_gamma_set(struct drm_crtc *crtc, u16 *red,
                                     u16 *green, u16 *blue,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
                                     uint32_t start,
#endif
                                     uint32_t size)
{
    /* Dummy */
}

static void vboxvideo_crtc_destroy(struct drm_crtc *crtc)
{
    struct vboxvideo_crtc *vboxvideo_crtc = to_vboxvideo_crtc(crtc);

    drm_crtc_cleanup(crtc);
    kfree(vboxvideo_crtc);
}

static const struct drm_crtc_funcs vboxvideo_crtc_funcs = {
    /*
    .cursor_set = vboxvideo_crtc_cursor_set,
    .cursor_move = vboxvideo_crtc_cursor_move,
    */
    .cursor_set = NULL,
    .cursor_move = NULL,
    .gamma_set = vboxvideo_crtc_gamma_set,
    .set_config = drm_crtc_helper_set_config,
    .destroy = vboxvideo_crtc_destroy,
};

static const struct drm_crtc_helper_funcs vboxvideo_helper_funcs = {
    .dpms = vboxvideo_crtc_dpms,
    .mode_fixup = vboxvideo_crtc_mode_fixup,
    .mode_set = vboxvideo_crtc_mode_set,
    .prepare = vboxvideo_crtc_prepare,
    .commit = vboxvideo_crtc_commit,
    .load_lut = vboxvideo_crtc_load_lut,
};

void vboxvideo_crtc_init(struct drm_device *dev, int index)
{
    struct vboxvideo_device *gdev = dev->dev_private;
    struct vboxvideo_crtc *vboxvideo_crtc;
    int i;

    vboxvideo_crtc = kzalloc(  sizeof(struct vboxvideo_crtc)
                             + (VBOXVIDEOFB_CONN_LIMIT
                                * sizeof(struct drm_connector *)),
                             GFP_KERNEL);
    if (vboxvideo_crtc == NULL)
        return;

    drm_crtc_init(dev, &vboxvideo_crtc->base, &vboxvideo_crtc_funcs);

    vboxvideo_crtc->crtc_id = index;
    if (gdev->fHaveHGSMI)
    {
        vboxvideo_crtc->offCommandBuffer =   gdev->offViewInfo
                                           - (index + 1) * VBVA_MIN_BUFFER_SIZE;
        VBoxVBVASetupBufferContext(&vboxvideo_crtc->VbvaCtx,
                                   vboxvideo_crtc->offCommandBuffer,
                                   VBVA_MIN_BUFFER_SIZE);
    }
    vboxvideo_crtc->last_dpms = VBOXVIDEO_DPMS_CLEARED;
    gdev->mode_info.crtcs[index] = vboxvideo_crtc;

    drm_crtc_helper_add(&vboxvideo_crtc->base, &vboxvideo_helper_funcs);
}

/** Sets the color ramps on behalf of fbcon */
void vboxvideo_crtc_fb_gamma_set(struct drm_crtc *crtc, u16 red, u16 green,
                                 u16 blue, int regno)
{

}

/** Gets the color ramps on behalf of fbcon */
void vboxvideo_crtc_fb_gamma_get(struct drm_crtc *crtc, u16 *red, u16 *green,
                                 u16 *blue, int regno)
{

}
