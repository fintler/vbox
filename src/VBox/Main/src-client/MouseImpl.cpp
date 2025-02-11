/* $Id$ */
/** @file
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include <iprt/cpp/utils.h>

#include "MouseImpl.h"
#include "DisplayImpl.h"
#include "VMMDev.h"

#include "AutoCaller.h"
#include "Logging.h"

#include <VBox/vmm/pdmdrv.h>
#include <VBox/VMMDev.h>

#include <iprt/asm.h>

/** @name Mouse device capabilities bitfield
 * @{ */
enum
{
    /** The mouse device can do relative reporting */
    MOUSE_DEVCAP_RELATIVE = 1,
    /** The mouse device can do absolute reporting */
    MOUSE_DEVCAP_ABSOLUTE = 2,
    /** The mouse device can do absolute reporting */
    MOUSE_DEVCAP_MULTI_TOUCH = 4
};
/** @} */


/**
 * Mouse driver instance data.
 */
struct DRVMAINMOUSE
{
    /** Pointer to the mouse object. */
    Mouse                      *pMouse;
    /** Pointer to the driver instance structure. */
    PPDMDRVINS                  pDrvIns;
    /** Pointer to the mouse port interface of the driver/device above us. */
    PPDMIMOUSEPORT              pUpPort;
    /** Our mouse connector interface. */
    PDMIMOUSECONNECTOR          IConnector;
    /** The capabilities of this device. */
    uint32_t                    u32DevCaps;
};


// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

Mouse::Mouse()
    : mParent(NULL)
{
}

Mouse::~Mouse()
{
}


HRESULT Mouse::FinalConstruct()
{
    RT_ZERO(mpDrv);
    mcLastX = 0x8000;
    mcLastY = 0x8000;
    mfLastButtons = 0;
    mfVMMDevGuestCaps = 0;
    return BaseFinalConstruct();
}

void Mouse::FinalRelease()
{
    uninit();
    BaseFinalRelease();
}

// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

/**
 * Initializes the mouse object.
 *
 * @returns COM result indicator
 * @param parent handle of our parent object
 */
HRESULT Mouse::init (ConsoleMouseInterface *parent)
{
    LogFlowThisFunc(("\n"));

    ComAssertRet(parent, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    unconst(mParent) = parent;

    unconst(mEventSource).createObject();
    HRESULT rc = mEventSource->init(static_cast<IMouse*>(this));
    AssertComRCReturnRC(rc);
    mMouseEvent.init(mEventSource, VBoxEventType_OnGuestMouse,
                     0, 0, 0, 0, 0);

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Uninitializes the instance and sets the ready flag to FALSE.
 *  Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void Mouse::uninit()
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    for (unsigned i = 0; i < MOUSE_MAX_DEVICES; ++i)
    {
        if (mpDrv[i])
            mpDrv[i]->pMouse = NULL;
        mpDrv[i] = NULL;
    }

    mMouseEvent.uninit();
    unconst(mEventSource).setNull();
    unconst(mParent) = NULL;
}


// IMouse properties
/////////////////////////////////////////////////////////////////////////////

/** Report the front-end's mouse handling capabilities to the VMM device and
 * thus to the guest.
 * @note all calls out of this object are made with no locks held! */
HRESULT Mouse::updateVMMDevMouseCaps(uint32_t fCapsAdded,
                                     uint32_t fCapsRemoved)
{
    VMMDevMouseInterface *pVMMDev = mParent->getVMMDevMouseInterface();
    if (!pVMMDev)
        return E_FAIL;  /* No assertion, as the front-ends can send events
                         * at all sorts of inconvenient times. */
    PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
    if (!pVMMDevPort)
        return E_FAIL;  /* same here */

    int rc = pVMMDevPort->pfnUpdateMouseCapabilities(pVMMDevPort, fCapsAdded,
                                                     fCapsRemoved);
    return RT_SUCCESS(rc) ? S_OK : E_FAIL;
}

/**
 * Returns whether the currently active device portfolio can accept absolute
 * mouse events.
 *
 * @returns COM status code
 * @param absoluteSupported address of result variable
 */
STDMETHODIMP Mouse::COMGETTER(AbsoluteSupported) (BOOL *absoluteSupported)
{
    if (!absoluteSupported)
        return E_POINTER;

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    *absoluteSupported = supportsAbs();
    return S_OK;
}

/**
 * Returns whether the currently active device portfolio can accept relative
 * mouse events.
 *
 * @returns COM status code
 * @param relativeSupported address of result variable
 */
STDMETHODIMP Mouse::COMGETTER(RelativeSupported) (BOOL *relativeSupported)
{
    if (!relativeSupported)
        return E_POINTER;

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    *relativeSupported = supportsRel();
    return S_OK;
}

/**
 * Returns whether the currently active device portfolio can accept multi-touch
 * mouse events.
 *
 * @returns COM status code
 * @param multiTouchSupported address of result variable
 */
STDMETHODIMP Mouse::COMGETTER(MultiTouchSupported) (BOOL *multiTouchSupported)
{
    if (!multiTouchSupported)
        return E_POINTER;

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    *multiTouchSupported = supportsMT();
    return S_OK;
}

/**
 * Returns whether the guest can currently switch to drawing the mouse cursor
 * itself if it is asked to by the front-end.
 *
 * @returns COM status code
 * @param pfNeedsHostCursor address of result variable
 */
STDMETHODIMP Mouse::COMGETTER(NeedsHostCursor) (BOOL *pfNeedsHostCursor)
{
    if (!pfNeedsHostCursor)
        return E_POINTER;

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    *pfNeedsHostCursor = guestNeedsHostCursor();
    return S_OK;
}

// IMouse methods
/////////////////////////////////////////////////////////////////////////////

/** Converts a bitfield containing information about mouse buttons currently
 * held down from the format used by the front-end to the format used by PDM
 * and the emulated pointing devices. */
static uint32_t mouseButtonsToPDM(LONG buttonState)
{
    uint32_t fButtons = 0;
    if (buttonState & MouseButtonState_LeftButton)
        fButtons |= PDMIMOUSEPORT_BUTTON_LEFT;
    if (buttonState & MouseButtonState_RightButton)
        fButtons |= PDMIMOUSEPORT_BUTTON_RIGHT;
    if (buttonState & MouseButtonState_MiddleButton)
        fButtons |= PDMIMOUSEPORT_BUTTON_MIDDLE;
    if (buttonState & MouseButtonState_XButton1)
        fButtons |= PDMIMOUSEPORT_BUTTON_X1;
    if (buttonState & MouseButtonState_XButton2)
        fButtons |= PDMIMOUSEPORT_BUTTON_X2;
    return fButtons;
}

STDMETHODIMP Mouse::COMGETTER(EventSource)(IEventSource ** aEventSource)
{
    CheckComArgOutPointerValid(aEventSource);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    // no need to lock - lifetime constant
    mEventSource.queryInterfaceTo(aEventSource);

    return S_OK;
}

/**
 * Send a relative pointer event to the relative device we deem most
 * appropriate.
 *
 * @returns   COM status code
 */
HRESULT Mouse::reportRelEventToMouseDev(int32_t dx, int32_t dy, int32_t dz,
                                        int32_t dw, uint32_t fButtons)
{
    if (dx || dy || dz || dw || fButtons != mfLastButtons)
    {
        PPDMIMOUSEPORT pUpPort = NULL;
        {
            AutoReadLock aLock(this COMMA_LOCKVAL_SRC_POS);

            for (unsigned i = 0; !pUpPort && i < MOUSE_MAX_DEVICES; ++i)
            {
                if (mpDrv[i] && (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_RELATIVE))
                    pUpPort = mpDrv[i]->pUpPort;
            }
        }
        if (!pUpPort)
            return S_OK;

        int vrc = pUpPort->pfnPutEvent(pUpPort, dx, dy, dz, dw, fButtons);

        if (RT_FAILURE(vrc))
            return setError(VBOX_E_IPRT_ERROR,
                            tr("Could not send the mouse event to the virtual mouse (%Rrc)"),
                            vrc);
        mfLastButtons = fButtons;
    }
    return S_OK;
}


/**
 * Send an absolute pointer event to the emulated absolute device we deem most
 * appropriate.
 *
 * @returns   COM status code
 */
HRESULT Mouse::reportAbsEventToMouseDev(int32_t x, int32_t y,
                                        int32_t dz, int32_t dw, uint32_t fButtons)
{
    if (   x < VMMDEV_MOUSE_RANGE_MIN
        || x > VMMDEV_MOUSE_RANGE_MAX)
        return S_OK;
    if (   y < VMMDEV_MOUSE_RANGE_MIN
        || y > VMMDEV_MOUSE_RANGE_MAX)
        return S_OK;
    if (   x != mcLastX || y != mcLastY
        || dz || dw || fButtons != mfLastButtons)
    {
        PPDMIMOUSEPORT pUpPort = NULL;
        {
            AutoReadLock aLock(this COMMA_LOCKVAL_SRC_POS);

            for (unsigned i = 0; !pUpPort && i < MOUSE_MAX_DEVICES; ++i)
            {
                if (mpDrv[i] && (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_ABSOLUTE))
                    pUpPort = mpDrv[i]->pUpPort;
            }
        }
        if (!pUpPort)
            return S_OK;

        int vrc = pUpPort->pfnPutEventAbs(pUpPort, x, y, dz,
                                          dw, fButtons);
        if (RT_FAILURE(vrc))
            return setError(VBOX_E_IPRT_ERROR,
                            tr("Could not send the mouse event to the virtual mouse (%Rrc)"),
                            vrc);
        mfLastButtons = fButtons;

    }
    return S_OK;
}

HRESULT Mouse::reportMultiTouchEventToDevice(uint8_t cContacts,
                                             const uint64_t *pau64Contacts,
                                             uint32_t u32ScanTime)
{
    HRESULT hrc = S_OK;

    PPDMIMOUSEPORT pUpPort = NULL;
    {
        AutoReadLock aLock(this COMMA_LOCKVAL_SRC_POS);

        unsigned i;
        for (i = 0; i < MOUSE_MAX_DEVICES; ++i)
        {
            if (   mpDrv[i]
                && (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_MULTI_TOUCH))
            {
                pUpPort = mpDrv[i]->pUpPort;
                break;
            }
        }
    }

    if (pUpPort)
    {
        int vrc = pUpPort->pfnPutEventMultiTouch(pUpPort, cContacts, pau64Contacts, u32ScanTime);
        if (RT_FAILURE(vrc))
            hrc = setError(VBOX_E_IPRT_ERROR,
                           tr("Could not send the multi-touch event to the virtual device (%Rrc)"),
                           vrc);
    }
    else
    {
        hrc = E_UNEXPECTED;
    }

    return hrc;
}


/**
 * Send an absolute position event to the VMM device.
 * @note all calls out of this object are made with no locks held!
 *
 * @returns   COM status code
 */
HRESULT Mouse::reportAbsEventToVMMDev(int32_t x, int32_t y)
{
    VMMDevMouseInterface *pVMMDev = mParent->getVMMDevMouseInterface();
    ComAssertRet(pVMMDev, E_FAIL);
    PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
    ComAssertRet(pVMMDevPort, E_FAIL);

    if (x != mcLastX || y != mcLastY)
    {
        int vrc = pVMMDevPort->pfnSetAbsoluteMouse(pVMMDevPort,
                                                   x, y);
        if (RT_FAILURE(vrc))
            return setError(VBOX_E_IPRT_ERROR,
                            tr("Could not send the mouse event to the virtual mouse (%Rrc)"),
                            vrc);
    }
    return S_OK;
}


/**
 * Send an absolute pointer event to a pointing device (the VMM device if
 * possible or whatever emulated absolute device seems best to us if not).
 *
 * @returns   COM status code
 */
HRESULT Mouse::reportAbsEvent(int32_t x, int32_t y,
                              int32_t dz, int32_t dw, uint32_t fButtons,
                              bool fUsesVMMDevEvent)
{
    HRESULT rc = S_OK;
    /** If we are using the VMMDev to report absolute position but without
     * VMMDev IRQ support then we need to send a small "jiggle" to the emulated
     * relative mouse device to alert the guest to changes. */
    LONG cJiggle = 0;

    /*
     * Send the absolute mouse position to the device.
     */
    if (x != mcLastX || y != mcLastY)
    {
        if (vmmdevCanAbs())
        {
            rc = reportAbsEventToVMMDev(x, y);
            cJiggle = !fUsesVMMDevEvent;
        }
        else
        {
            rc = reportAbsEventToMouseDev(x, y, 0, 0, fButtons);
            fButtons = 0;
        }
    }
    if (SUCCEEDED(rc))
        rc = reportRelEventToMouseDev(cJiggle, 0, dz, dw, fButtons);

    mcLastX = x;
    mcLastY = y;
    return rc;
}

void Mouse::fireMouseEvent(bool fAbsolute, LONG x, LONG y, LONG dz, LONG dw,
                           LONG cContact, LONG fButtons)
{
    /* If mouse button is pressed, we generate new event, to avoid reusable events coalescing and thus
       dropping key press events */
    if (fButtons != 0)
    {
        VBoxEventDesc evDesc;
        evDesc.init(mEventSource, VBoxEventType_OnGuestMouse, fAbsolute, x, y,
                    dz, dw, cContact, fButtons);
        evDesc.fire(0);
    }
    else
    {
        mMouseEvent.reinit(VBoxEventType_OnGuestMouse, fAbsolute, x, y, dz, dw,
                           cContact, fButtons);
        mMouseEvent.fire(0);
    }
}


/**
 * Send a relative mouse event to the guest.
 * @note the VMMDev capability change is so that the guest knows we are sending
 *       real events over the PS/2 device and not dummy events to signal the
 *       arrival of new absolute pointer data
 *
 * @returns COM status code
 * @param dx          X movement
 * @param dy          Y movement
 * @param dz          Z movement
 * @param fButtons    The mouse button state
 */
STDMETHODIMP Mouse::PutMouseEvent(LONG dx, LONG dy, LONG dz, LONG dw,
                                  LONG fButtons)
{
    HRESULT rc;
    uint32_t fButtonsAdj;

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();
    LogRel3(("%s: dx=%d, dy=%d, dz=%d, dw=%d\n", __PRETTY_FUNCTION__,
                 dx, dy, dz, dw));

    fButtonsAdj = mouseButtonsToPDM(fButtons);
    /* Make sure that the guest knows that we are sending real movement
     * events to the PS/2 device and not just dummy wake-up ones. */
    updateVMMDevMouseCaps(0, VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE);
    rc = reportRelEventToMouseDev(dx, dy, dz, dw, fButtonsAdj);

    fireMouseEvent(false, dx, dy, dz, dw, 0, fButtons);

    return rc;
}

/**
 * Convert an (X, Y) value pair in screen co-ordinates (starting from 1) to a
 * value from VMMDEV_MOUSE_RANGE_MIN to VMMDEV_MOUSE_RANGE_MAX.  Sets the
 * optional validity value to false if the pair is not on an active screen and
 * to true otherwise.
 * @note      since guests with recent versions of X.Org use a different method
 *            to everyone else to map the valuator value to a screen pixel (they
 *            multiply by the screen dimension, do a floating point divide by
 *            the valuator maximum and round the result, while everyone else
 *            does truncating integer operations) we adjust the value we send
 *            so that it maps to the right pixel both when the result is rounded
 *            and when it is truncated.
 *
 * @returns   COM status value
 */
HRESULT Mouse::convertDisplayRes(LONG x, LONG y, int32_t *pxAdj, int32_t *pyAdj,
                                 bool *pfValid)
{
    AssertPtrReturn(pxAdj, E_POINTER);
    AssertPtrReturn(pyAdj, E_POINTER);
    AssertPtrNullReturn(pfValid, E_POINTER);
    DisplayMouseInterface *pDisplay = mParent->getDisplayMouseInterface();
    ComAssertRet(pDisplay, E_FAIL);
    /** The amount to add to the result (multiplied by the screen width/height)
     * to compensate for differences in guest methods for mapping back to
     * pixels */
    enum { ADJUST_RANGE = - 3 * VMMDEV_MOUSE_RANGE / 4 };

    if (pfValid)
        *pfValid = true;
    if (!(mfVMMDevGuestCaps & VMMDEV_MOUSE_NEW_PROTOCOL))
    {
        ULONG displayWidth, displayHeight;
        /* Takes the display lock */
        HRESULT rc = pDisplay->getScreenResolution(0, &displayWidth,
                                                   &displayHeight, NULL);
        if (FAILED(rc))
            return rc;

        *pxAdj = displayWidth ?   (x * VMMDEV_MOUSE_RANGE + ADJUST_RANGE)
                                / (LONG) displayWidth: 0;
        *pyAdj = displayHeight ?   (y * VMMDEV_MOUSE_RANGE + ADJUST_RANGE)
                                 / (LONG) displayHeight: 0;
    }
    else
    {
        int32_t x1, y1, x2, y2;
        /* Takes the display lock */
        pDisplay->getFramebufferDimensions(&x1, &y1, &x2, &y2);
        *pxAdj = x1 < x2 ?   ((x - x1) * VMMDEV_MOUSE_RANGE + ADJUST_RANGE)
                           / (x2 - x1) : 0;
        *pyAdj = y1 < y2 ?   ((y - y1) * VMMDEV_MOUSE_RANGE + ADJUST_RANGE)
                           / (y2 - y1) : 0;
        if (   *pxAdj < VMMDEV_MOUSE_RANGE_MIN
            || *pxAdj > VMMDEV_MOUSE_RANGE_MAX
            || *pyAdj < VMMDEV_MOUSE_RANGE_MIN
            || *pyAdj > VMMDEV_MOUSE_RANGE_MAX)
            if (pfValid)
                *pfValid = false;
    }
    return S_OK;
}


/**
 * Send an absolute mouse event to the VM. This requires either VirtualBox-
 * specific drivers installed in the guest or absolute pointing device
 * emulation.
 * @note the VMMDev capability change is so that the guest knows we are sending
 *       dummy events over the PS/2 device to signal the arrival of new
 *       absolute pointer data, and not pointer real movement data
 * @note all calls out of this object are made with no locks held!
 *
 * @returns COM status code
 * @param x          X position (pixel), starting from 1
 * @param y          Y position (pixel), starting from 1
 * @param dz         Z movement
 * @param fButtons   The mouse button state
 */
STDMETHODIMP Mouse::PutMouseEventAbsolute(LONG x, LONG y, LONG dz, LONG dw,
                                          LONG fButtons)
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    LogRel3(("%s: x=%d, y=%d, dz=%d, dw=%d, fButtons=0x%x\n",
             __PRETTY_FUNCTION__, x, y, dz, dw, fButtons));

    int32_t xAdj, yAdj;
    uint32_t fButtonsAdj;
    bool fValid;

    /** @todo the front end should do this conversion to avoid races */
    /** @note Or maybe not... races are pretty inherent in everything done in
     *        this object and not really bad as far as I can see. */
    HRESULT rc = convertDisplayRes(x, y, &xAdj, &yAdj, &fValid);
    if (FAILED(rc)) return rc;

    fButtonsAdj = mouseButtonsToPDM(fButtons);
    /* If we are doing old-style (IRQ-less) absolute reporting to the VMM
     * device then make sure the guest is aware of it, so that it knows to
     * ignore relative movement on the PS/2 device. */
    updateVMMDevMouseCaps(VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE, 0);
    if (fValid)
    {
        rc = reportAbsEvent(xAdj, yAdj, dz, dw, fButtonsAdj,
                            RT_BOOL(  mfVMMDevGuestCaps
                                    & VMMDEV_MOUSE_NEW_PROTOCOL));

        fireMouseEvent(true, x, y, dz, dw, 0, fButtons);
    }

    return rc;
}

/**
 * Send a multi-touch event. This requires multi-touch pointing device emulation.
 * @note all calls out of this object are made with no locks held!
 *
 * @returns COM status code.
 * @param aCount     Number of contacts.
 * @param aContacts  Information about each contact.
 * @param aScanTime  Timestamp.
 */
STDMETHODIMP Mouse::PutEventMultiTouch(LONG aCount,
                                       ComSafeArrayIn(LONG64, aContacts),
                                       ULONG aScanTime)
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    com::SafeArray <LONG64> arrayContacts(ComSafeArrayInArg(aContacts));

    LogRel3(("%s: aCount %d(actual %d), aScanTime %u\n",
             __FUNCTION__, aCount, arrayContacts.size(), aScanTime));

    HRESULT rc = S_OK;

    if ((LONG)arrayContacts.size() >= aCount)
    {
        LONG64* paContacts = arrayContacts.raw();

        rc = putEventMultiTouch(aCount, paContacts, aScanTime);
    }
    else
    {
        rc = E_INVALIDARG;
    }

    return rc;
}

/**
 * Send a multi-touch event. Version for scripting languages.
 *
 * @returns COM status code.
 * @param aCount     Number of contacts.
 * @param aContacts  Information about each contact.
 * @param aScanTime  Timestamp.
 */
STDMETHODIMP Mouse::PutEventMultiTouchString(LONG aCount,
                                             IN_BSTR aContacts,
                                             ULONG aScanTime)
{
    /** @todo implement: convert the string to LONG64 array and call putEventMultiTouch. */
    NOREF(aCount);
    NOREF(aContacts);
    NOREF(aScanTime);
    return E_NOTIMPL;
}


// private methods
/////////////////////////////////////////////////////////////////////////////

/* Used by PutEventMultiTouch and PutEventMultiTouchString. */
HRESULT Mouse::putEventMultiTouch(LONG aCount,
                                  LONG64 *paContacts,
                                  ULONG aScanTime)
{
    if (aCount >= 256)
    {
         return E_INVALIDARG;
    }

    DisplayMouseInterface *pDisplay = mParent->getDisplayMouseInterface();
    ComAssertRet(pDisplay, E_FAIL);

    HRESULT rc = S_OK;

    uint64_t* pau64Contacts = NULL;
    uint8_t cContacts = 0;

    /* Deliver 0 contacts too, touch device may use this to reset the state. */
    if (aCount > 0)
    {
        /* Create a copy with converted coords. */
        pau64Contacts = (uint64_t *)RTMemTmpAlloc(aCount * sizeof(uint64_t));
        if (pau64Contacts)
        {
            int32_t x1, y1, x2, y2;
            /* Takes the display lock */
            pDisplay->getFramebufferDimensions(&x1, &y1, &x2, &y2);

            LONG i;
            for (i = 0; i < aCount; i++)
            {
                uint32_t u32Lo = RT_LO_U32(paContacts[i]);
                uint32_t u32Hi = RT_HI_U32(paContacts[i]);
                int32_t x = (int16_t)u32Lo;
                int32_t y = (int16_t)(u32Lo >> 16);
                uint8_t contactId =  RT_BYTE1(u32Hi);
                bool fInContact   = (RT_BYTE2(u32Hi) & 0x1) != 0;
                bool fInRange     = (RT_BYTE2(u32Hi) & 0x2) != 0;

                LogRel3(("%s: [%d] %d,%d id %d, inContact %d, inRange %d\n",
                         __FUNCTION__, i, x, y, contactId, fInContact, fInRange));

                /* Framebuffer dimensions are 0,0 width, height, that is x2,y2 are exclusive,
                 * while coords are inclusive.
                 */
                int32_t xAdj = x1 < x2 ?   ((x - 1 - x1) * VMMDEV_MOUSE_RANGE)
                                         / (x2 - x1) : 0;
                int32_t yAdj = y1 < y2 ?   ((y - 1 - y1) * VMMDEV_MOUSE_RANGE)
                                         / (y2 - y1) : 0;

                bool fValid = (   xAdj >= VMMDEV_MOUSE_RANGE_MIN
                               && xAdj <= VMMDEV_MOUSE_RANGE_MAX
                               && yAdj >= VMMDEV_MOUSE_RANGE_MIN
                               && yAdj <= VMMDEV_MOUSE_RANGE_MAX);

                if (fValid)
                {
                    uint8_t fu8 =   (fInContact? 0x01: 0x00)
                                  | (fInRange?   0x02: 0x00);
                    pau64Contacts[cContacts] = RT_MAKE_U64_FROM_U16((uint16_t)xAdj,
                                                                    (uint16_t)yAdj,
                                                                    RT_MAKE_U16(contactId, fu8),
                                                                    0);
                    cContacts++;
                }
            }
        }
        else
        {
            rc = E_OUTOFMEMORY;
        }
    }

    if (SUCCEEDED(rc))
    {
        rc = reportMultiTouchEventToDevice(cContacts, cContacts? pau64Contacts: NULL, (uint32_t)aScanTime);

        // @todo Implement. Maybe using a new TouchEvent rather than extending the mouse event.
        // fireMouseEvent(true, x, y, 0, 0, cContact, contactState);
    }

    RTMemTmpFree(pau64Contacts);

    return rc;
}


/** Does the guest currently rely on the host to draw the mouse cursor or
 * can it switch to doing it itself in software? */
bool Mouse::guestNeedsHostCursor(void)
{
    return RT_BOOL(mfVMMDevGuestCaps & VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR);
}


/** Check what sort of reporting can be done using the devices currently
 * enabled.  Does not consider the VMM device. */
void Mouse::getDeviceCaps(bool *pfAbs, bool *pfRel, bool *pfMT)
{
    bool fAbsDev = false;
    bool fRelDev = false;
    bool fMTDev  = false;

    AutoReadLock aLock(this COMMA_LOCKVAL_SRC_POS);

    for (unsigned i = 0; i < MOUSE_MAX_DEVICES; ++i)
        if (mpDrv[i])
        {
           if (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_ABSOLUTE)
               fAbsDev = true;
           if (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_RELATIVE)
               fRelDev = true;
           if (mpDrv[i]->u32DevCaps & MOUSE_DEVCAP_MULTI_TOUCH)
               fMTDev  = true;
        }
    if (pfAbs)
        *pfAbs = fAbsDev;
    if (pfRel)
        *pfRel = fRelDev;
    if (pfMT)
        *pfMT = fMTDev;
}


/** Does the VMM device currently support absolute reporting? */
bool Mouse::vmmdevCanAbs(void)
{
    bool fRelDev;

    getDeviceCaps(NULL, &fRelDev, NULL);
    return    (mfVMMDevGuestCaps & VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE)
           && fRelDev;
}


/** Does the VMM device currently support absolute reporting? */
bool Mouse::deviceCanAbs(void)
{
    bool fAbsDev;

    getDeviceCaps(&fAbsDev, NULL, NULL);
    return fAbsDev;
}


/** Can we currently send relative events to the guest? */
bool Mouse::supportsRel(void)
{
    bool fRelDev;

    getDeviceCaps(NULL, &fRelDev, NULL);
    return fRelDev;
}


/** Can we currently send absolute events to the guest? */
bool Mouse::supportsAbs(void)
{
    bool fAbsDev;

    getDeviceCaps(&fAbsDev, NULL, NULL);
    return fAbsDev || vmmdevCanAbs();
}


/** Can we currently send absolute events to the guest? */
bool Mouse::supportsMT(void)
{
    bool fMTDev;

    getDeviceCaps(NULL, NULL, &fMTDev);
    return fMTDev;
}


/** Check what sort of reporting can be done using the devices currently
 * enabled (including the VMM device) and notify the guest and the front-end.
 */
void Mouse::sendMouseCapsNotifications(void)
{
    bool fAbsDev, fRelDev, fMTDev, fCanAbs, fNeedsHostCursor;

    {
        AutoReadLock aLock(this COMMA_LOCKVAL_SRC_POS);

        getDeviceCaps(&fAbsDev, &fRelDev, &fMTDev);
        fCanAbs = supportsAbs();
        fNeedsHostCursor = guestNeedsHostCursor();
    }
    if (fAbsDev)
        updateVMMDevMouseCaps(VMMDEV_MOUSE_HOST_HAS_ABS_DEV, 0);
    else
        updateVMMDevMouseCaps(0, VMMDEV_MOUSE_HOST_HAS_ABS_DEV);
    /** @todo this call takes the Console lock in order to update the cached
     * callback data atomically.  However I can't see any sign that the cached
     * data is ever used again. */
    mParent->onMouseCapabilityChange(fCanAbs, fRelDev, fMTDev, fNeedsHostCursor);
}


/**
 * @interface_method_impl{PDMIMOUSECONNECTOR,pfnReportModes}
 * A virtual device is notifying us about its current state and capabilities
 */
DECLCALLBACK(void) Mouse::mouseReportModes(PPDMIMOUSECONNECTOR pInterface, bool fRel, bool fAbs, bool fMT)
{
    PDRVMAINMOUSE pDrv = RT_FROM_MEMBER(pInterface, DRVMAINMOUSE, IConnector);
    if (fRel)
        pDrv->u32DevCaps |= MOUSE_DEVCAP_RELATIVE;
    else
        pDrv->u32DevCaps &= ~MOUSE_DEVCAP_RELATIVE;
    if (fAbs)
        pDrv->u32DevCaps |= MOUSE_DEVCAP_ABSOLUTE;
    else
        pDrv->u32DevCaps &= ~MOUSE_DEVCAP_ABSOLUTE;
    if (fMT)
        pDrv->u32DevCaps |= MOUSE_DEVCAP_MULTI_TOUCH;
    else
        pDrv->u32DevCaps &= ~MOUSE_DEVCAP_MULTI_TOUCH;

    pDrv->pMouse->sendMouseCapsNotifications();
}


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
DECLCALLBACK(void *)  Mouse::drvQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS      pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVMAINMOUSE   pDrv    = PDMINS_2_DATA(pDrvIns, PDRVMAINMOUSE);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMOUSECONNECTOR, &pDrv->IConnector);
    return NULL;
}


/**
 * Destruct a mouse driver instance.
 *
 * @returns VBox status.
 * @param   pDrvIns     The driver instance data.
 */
DECLCALLBACK(void) Mouse::drvDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVMAINMOUSE pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINMOUSE);
    LogFlow(("Mouse::drvDestruct: iInstance=%d\n", pDrvIns->iInstance));

    if (pThis->pMouse)
    {
        AutoWriteLock mouseLock(pThis->pMouse COMMA_LOCKVAL_SRC_POS);
        for (unsigned cDev = 0; cDev < MOUSE_MAX_DEVICES; ++cDev)
            if (pThis->pMouse->mpDrv[cDev] == pThis)
            {
                pThis->pMouse->mpDrv[cDev] = NULL;
                break;
            }
    }
}


/**
 * Construct a mouse driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
DECLCALLBACK(int) Mouse::drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVMAINMOUSE pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINMOUSE);
    LogFlow(("drvMainMouse_Construct: iInstance=%d\n", pDrvIns->iInstance));

    /*
     * Validate configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg, "Object\0"))
        return VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES;
    AssertMsgReturn(PDMDrvHlpNoAttach(pDrvIns) == VERR_PDM_NO_ATTACHED_DRIVER,
                    ("Configuration error: Not possible to attach anything to this driver!\n"),
                    VERR_PDM_DRVINS_NO_ATTACH);

    /*
     * IBase.
     */
    pDrvIns->IBase.pfnQueryInterface        = Mouse::drvQueryInterface;

    pThis->IConnector.pfnReportModes        = Mouse::mouseReportModes;

    /*
     * Get the IMousePort interface of the above driver/device.
     */
    pThis->pUpPort = (PPDMIMOUSEPORT)pDrvIns->pUpBase->pfnQueryInterface(pDrvIns->pUpBase, PDMIMOUSEPORT_IID);
    if (!pThis->pUpPort)
    {
        AssertMsgFailed(("Configuration error: No mouse port interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }

    /*
     * Get the Mouse object pointer and update the mpDrv member.
     */
    void *pv;
    int rc = CFGMR3QueryPtr(pCfg, "Object", &pv);
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("Configuration error: No/bad \"Object\" value! rc=%Rrc\n", rc));
        return rc;
    }
    pThis->pMouse = (Mouse *)pv;        /** @todo Check this cast! */
    unsigned cDev;
    {
        AutoReadLock mouseLock(pThis->pMouse COMMA_LOCKVAL_SRC_POS);

        for (cDev = 0; cDev < MOUSE_MAX_DEVICES; ++cDev)
            if (!pThis->pMouse->mpDrv[cDev])
            {
                pThis->pMouse->mpDrv[cDev] = pThis;
                break;
            }
    }
    if (cDev == MOUSE_MAX_DEVICES)
        return VERR_NO_MORE_HANDLES;

    return VINF_SUCCESS;
}


/**
 * Main mouse driver registration record.
 */
const PDMDRVREG Mouse::DrvReg =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "MainMouse",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Main mouse driver (Main as in the API).",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_MOUSE,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVMAINMOUSE),
    /* pfnConstruct */
    Mouse::drvConstruct,
    /* pfnDestruct */
    Mouse::drvDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
