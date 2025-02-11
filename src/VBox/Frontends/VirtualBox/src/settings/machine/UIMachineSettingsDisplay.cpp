/* $Id$ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIMachineSettingsDisplay class implementation
 */

/*
 * Copyright (C) 2008-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QDesktopWidget>

/* GUI includes: */
#include "QIWidgetValidator.h"
#include "UIMachineSettingsDisplay.h"
#include "VBoxGlobal.h"
#include "UIMessageCenter.h"
#include "UIConverter.h"

/* COM includes: */
#include "CVRDEServer.h"

UIMachineSettingsDisplay::UIMachineSettingsDisplay()
    : m_iMinVRAM(0)
    , m_iMaxVRAM(0)
    , m_iMaxVRAMVisible(0)
    , m_iInitialVRAM(0)
#ifdef VBOX_WITH_VIDEOHWACCEL
    , m_f2DVideoAccelerationSupported(false)
#endif /* VBOX_WITH_VIDEOHWACCEL */
#ifdef VBOX_WITH_CRHGSMI
    , m_fWddmModeSupported(false)
#endif /* VBOX_WITH_CRHGSMI */
{
    /* Prepare: */
    prepare();
}

void UIMachineSettingsDisplay::setGuestOSType(CGuestOSType guestOSType)
{
    /* Check if guest os type changed: */
    if (m_guestOSType == guestOSType)
        return;

    /* Remember new guest os type: */
    m_guestOSType = guestOSType;

#ifdef VBOX_WITH_VIDEOHWACCEL
    /* Check if 2D video acceleration supported by the guest OS type: */
    QString strguestOSTypeFamily = m_guestOSType.GetFamilyId();
    m_f2DVideoAccelerationSupported = strguestOSTypeFamily == "Windows";
#endif /* VBOX_WITH_VIDEOHWACCEL */
#ifdef VBOX_WITH_CRHGSMI
    /* Check if WDDM mode supported by the guest OS type: */
    QString strguestOSTypeId = m_guestOSType.GetId();
    m_fWddmModeSupported = VBoxGlobal::isWddmCompatibleOsType(strguestOSTypeId);
#endif /* VBOX_WITH_CRHGSMI */

    /* Recheck video RAM requirement: */
    checkVRAMRequirements();

    /* Revalidate: */
    revalidate();
}

#ifdef VBOX_WITH_VIDEOHWACCEL
bool UIMachineSettingsDisplay::isAcceleration2DVideoSelected() const
{
    return m_pCheckbox2DVideo->isChecked();
}
#endif /* VBOX_WITH_VIDEOHWACCEL */

/* Load data to cache from corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void UIMachineSettingsDisplay::loadToCacheFrom(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Clear cache initially: */
    m_cache.clear();

    /* Prepare display data: */
    UIDataSettingsMachineDisplay displayData;

    /* Cache Video data: */
    displayData.m_iCurrentVRAM = m_machine.GetVRAMSize();
    displayData.m_cGuestScreenCount = m_machine.GetMonitorCount();
    displayData.m_f3dAccelerationEnabled = m_machine.GetAccelerate3DEnabled();
#ifdef VBOX_WITH_VIDEOHWACCEL
    displayData.m_f2dAccelerationEnabled = m_machine.GetAccelerate2DVideoEnabled();
#endif /* VBOX_WITH_VIDEOHWACCEL */

    /* Check if Remote Display server is valid: */
    CVRDEServer remoteDisplayServer = m_machine.GetVRDEServer();
    displayData.m_fRemoteDisplayServerSupported = !remoteDisplayServer.isNull();
    if (!remoteDisplayServer.isNull())
    {
        /* Cache Remote Display data: */
        displayData.m_fRemoteDisplayServerEnabled = remoteDisplayServer.GetEnabled();
        displayData.m_strRemoteDisplayPort = remoteDisplayServer.GetVRDEProperty("TCP/Ports");
        displayData.m_remoteDisplayAuthType = remoteDisplayServer.GetAuthType();
        displayData.m_uRemoteDisplayTimeout = remoteDisplayServer.GetAuthTimeout();
        displayData.m_fRemoteDisplayMultiConnAllowed = remoteDisplayServer.GetAllowMultiConnection();
    }

    /* Cache Video Capture data: */
    displayData.m_fVideoCaptureEnabled = m_machine.GetVideoCaptureEnabled();
    displayData.m_strVideoCaptureFolder = QFileInfo(m_machine.GetSettingsFilePath()).absolutePath();
    displayData.m_strVideoCaptureFilePath = m_machine.GetVideoCaptureFile();
    displayData.m_iVideoCaptureFrameWidth = m_machine.GetVideoCaptureWidth();
    displayData.m_iVideoCaptureFrameHeight = m_machine.GetVideoCaptureHeight();
    displayData.m_iVideoCaptureFrameRate = m_machine.GetVideoCaptureFPS();
    displayData.m_iVideoCaptureBitRate = m_machine.GetVideoCaptureRate();
    displayData.m_screens = m_machine.GetVideoCaptureScreens();

    /* Initialize other variables: */
    m_iInitialVRAM = RT_MIN(displayData.m_iCurrentVRAM, m_iMaxVRAM);

    /* Cache display data: */
    m_cache.cacheInitialData(displayData);

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

/* Load data to corresponding widgets from cache,
 * this task SHOULD be performed in GUI thread only: */
void UIMachineSettingsDisplay::getFromCache()
{
    /* Get display data from cache: */
    const UIDataSettingsMachineDisplay &displayData = m_cache.base();

    /* Load Video data to page: */
    m_pEditorVideoScreenCount->setValue(displayData.m_cGuestScreenCount);
    m_pCheckbox3D->setChecked(displayData.m_f3dAccelerationEnabled);
#ifdef VBOX_WITH_VIDEOHWACCEL
    m_pCheckbox2DVideo->setChecked(displayData.m_f2dAccelerationEnabled);
#endif /* VBOX_WITH_VIDEOHWACCEL */
    /* Should be the last one from this tab: */
    m_pEditorVideoMemorySize->setValue(displayData.m_iCurrentVRAM);

    /* If Remote Display server is supported: */
    if (displayData.m_fRemoteDisplayServerSupported)
    {
        /* Load Remote Display data to page: */
        m_pCheckboxRemoteDisplay->setChecked(displayData.m_fRemoteDisplayServerEnabled);
        m_pEditorRemoteDisplayPort->setText(displayData.m_strRemoteDisplayPort);
        m_pComboRemoteDisplayAuthMethod->setCurrentIndex(m_pComboRemoteDisplayAuthMethod->findText(gpConverter->toString(displayData.m_remoteDisplayAuthType)));
        m_pEditorRemoteDisplayTimeout->setText(QString::number(displayData.m_uRemoteDisplayTimeout));
        m_pCheckboxMultipleConn->setChecked(displayData.m_fRemoteDisplayMultiConnAllowed);
    }

    /* Load Video Capture data to page: */
    m_pCheckboxVideoCapture->setChecked(displayData.m_fVideoCaptureEnabled);
    m_pEditorVideoCapturePath->setHomeDir(displayData.m_strVideoCaptureFolder);
    m_pEditorVideoCapturePath->setPath(displayData.m_strVideoCaptureFilePath);
    m_pEditorVideoCaptureWidth->setValue(displayData.m_iVideoCaptureFrameWidth);
    m_pEditorVideoCaptureHeight->setValue(displayData.m_iVideoCaptureFrameHeight);
    m_pEditorVideoCaptureFrameRate->setValue(displayData.m_iVideoCaptureFrameRate);
    m_pEditorVideoCaptureBitRate->setValue(displayData.m_iVideoCaptureBitRate);
    m_pScrollerVideoCaptureScreens->setValue(displayData.m_screens);

    /* Polish page finally: */
    polishPage();

    /* Revalidate: */
    revalidate();
}

/* Save data from corresponding widgets to cache,
 * this task SHOULD be performed in GUI thread only: */
void UIMachineSettingsDisplay::putToCache()
{
    /* Prepare audio data: */
    UIDataSettingsMachineDisplay displayData = m_cache.base();

    /* Gather Video data from page: */
    displayData.m_iCurrentVRAM = m_pEditorVideoMemorySize->value();
    displayData.m_cGuestScreenCount = m_pEditorVideoScreenCount->value();
    displayData.m_f3dAccelerationEnabled = m_pCheckbox3D->isChecked();
#ifdef VBOX_WITH_VIDEOHWACCEL
    displayData.m_f2dAccelerationEnabled = m_pCheckbox2DVideo->isChecked();
#endif /* VBOX_WITH_VIDEOHWACCEL */

    /* If Remote Display server is supported: */
    if (displayData.m_fRemoteDisplayServerSupported)
    {
        /* Gather Remote Display data from page: */
        displayData.m_fRemoteDisplayServerEnabled = m_pCheckboxRemoteDisplay->isChecked();
        displayData.m_strRemoteDisplayPort = m_pEditorRemoteDisplayPort->text();
        displayData.m_remoteDisplayAuthType = gpConverter->fromString<KAuthType>(m_pComboRemoteDisplayAuthMethod->currentText());
        displayData.m_uRemoteDisplayTimeout = m_pEditorRemoteDisplayTimeout->text().toULong();
        displayData.m_fRemoteDisplayMultiConnAllowed = m_pCheckboxMultipleConn->isChecked();
    }

    /* Gather Video Capture data from page: */
    displayData.m_fVideoCaptureEnabled = m_pCheckboxVideoCapture->isChecked();
    displayData.m_strVideoCaptureFilePath = m_pEditorVideoCapturePath->path();
    displayData.m_iVideoCaptureFrameWidth = m_pEditorVideoCaptureWidth->value();
    displayData.m_iVideoCaptureFrameHeight = m_pEditorVideoCaptureHeight->value();
    displayData.m_iVideoCaptureFrameRate = m_pEditorVideoCaptureFrameRate->value();
    displayData.m_iVideoCaptureBitRate = m_pEditorVideoCaptureBitRate->value();
    displayData.m_screens = m_pScrollerVideoCaptureScreens->value();

    /* Cache display data: */
    m_cache.cacheCurrentData(displayData);
}

/* Save data from cache to corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void UIMachineSettingsDisplay::saveFromCacheTo(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Make sure machine is in valid mode & display data was changed: */
    if (isMachineInValidMode() && m_cache.wasChanged())
    {
        /* Get display data from cache: */
        const UIDataSettingsMachineDisplay &displayData = m_cache.data();

        /* Make sure machine is 'offline': */
        if (isMachineOffline())
        {
            /* Store Video data: */
            m_machine.SetVRAMSize(displayData.m_iCurrentVRAM);
            m_machine.SetMonitorCount(displayData.m_cGuestScreenCount);
            m_machine.SetAccelerate3DEnabled(displayData.m_f3dAccelerationEnabled);
#ifdef VBOX_WITH_VIDEOHWACCEL
            m_machine.SetAccelerate2DVideoEnabled(displayData.m_f2dAccelerationEnabled);
#endif /* VBOX_WITH_VIDEOHWACCEL */
        }

        /* Check if Remote Display server still valid: */
        CVRDEServer remoteDisplayServer = m_machine.GetVRDEServer();
        if (!remoteDisplayServer.isNull())
        {
            /* Store Remote Display data: */
            remoteDisplayServer.SetEnabled(displayData.m_fRemoteDisplayServerEnabled);
            remoteDisplayServer.SetVRDEProperty("TCP/Ports", displayData.m_strRemoteDisplayPort);
            remoteDisplayServer.SetAuthType(displayData.m_remoteDisplayAuthType);
            remoteDisplayServer.SetAuthTimeout(displayData.m_uRemoteDisplayTimeout);
            /* Make sure machine is 'offline' or 'saved': */
            if (isMachineOffline() || isMachineSaved())
                remoteDisplayServer.SetAllowMultiConnection(displayData.m_fRemoteDisplayMultiConnAllowed);
        }

        /* Store Video Capture data: */
        if (isMachineOnline())
        {
            /* If Video Capture is *enabled* now: */
            if (m_cache.base().m_fVideoCaptureEnabled)
            {
                /* We can still save the *screens* option: */
                m_machine.SetVideoCaptureScreens(displayData.m_screens);
                /* Finally we should *disable* Video Capture if necessary: */
                if (!displayData.m_fVideoCaptureEnabled)
                    m_machine.SetVideoCaptureEnabled(displayData.m_fVideoCaptureEnabled);
            }
            /* If Video Capture is *disabled* now: */
            else
            {
                /* We should save all the options *before* Video Capture activation: */
                m_machine.SetVideoCaptureFile(displayData.m_strVideoCaptureFilePath);
                m_machine.SetVideoCaptureWidth(displayData.m_iVideoCaptureFrameWidth);
                m_machine.SetVideoCaptureHeight(displayData.m_iVideoCaptureFrameHeight);
                m_machine.SetVideoCaptureFPS(displayData.m_iVideoCaptureFrameRate);
                m_machine.SetVideoCaptureRate(displayData.m_iVideoCaptureBitRate);
                m_machine.SetVideoCaptureScreens(displayData.m_screens);
                /* Finally we should *enable* Video Capture if necessary: */
                if (displayData.m_fVideoCaptureEnabled)
                    m_machine.SetVideoCaptureEnabled(displayData.m_fVideoCaptureEnabled);
            }
        }
        else
        {
            /* For 'offline' and 'saved' states the order is irrelevant: */
            m_machine.SetVideoCaptureEnabled(displayData.m_fVideoCaptureEnabled);
            m_machine.SetVideoCaptureFile(displayData.m_strVideoCaptureFilePath);
            m_machine.SetVideoCaptureWidth(displayData.m_iVideoCaptureFrameWidth);
            m_machine.SetVideoCaptureHeight(displayData.m_iVideoCaptureFrameHeight);
            m_machine.SetVideoCaptureFPS(displayData.m_iVideoCaptureFrameRate);
            m_machine.SetVideoCaptureRate(displayData.m_iVideoCaptureBitRate);
            m_machine.SetVideoCaptureScreens(displayData.m_screens);
        }
    }

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

bool UIMachineSettingsDisplay::validate(QString &strWarning, QString&)
{
    /* Check if video RAM requirement changed first: */
    checkVRAMRequirements();

    if (m_pCheckbox3D->isChecked() && !vboxGlobal().is3DAvailable())
    {
        strWarning = tr("you enabled 3D acceleration. However, 3D acceleration is not "
                        "working on the current host setup so you will not be able to "
                        "start the VM.");
        return true;
    }

    /* Video RAM amount test: */
    if (shouldWeWarnAboutLowVideoMemory() && !m_guestOSType.isNull())
    {
        quint64 uNeedBytes = VBoxGlobal::requiredVideoMemory(m_guestOSType.GetId(), m_pEditorVideoScreenCount->value());

        /* Basic video RAM amount test: */
        if ((quint64)m_pEditorVideoMemorySize->value() * _1M < uNeedBytes)
        {
            strWarning = tr("you have assigned less than <b>%1</b> of video memory which is "
                            "the minimum amount required to switch the virtual machine to "
                            "fullscreen or seamless mode.")
                            .arg(vboxGlobal().formatSize(uNeedBytes, 0, FormatSize_RoundUp));
            return true;
        }
#ifdef VBOX_WITH_VIDEOHWACCEL
        /* 2D acceleration video RAM amount test: */
        if (m_pCheckbox2DVideo->isChecked() && m_f2DVideoAccelerationSupported)
        {
            uNeedBytes += VBoxGlobal::required2DOffscreenVideoMemory();
            if ((quint64)m_pEditorVideoMemorySize->value() * _1M < uNeedBytes)
            {
                strWarning = tr("you have assigned less than <b>%1</b> of video memory which is "
                                "the minimum amount required for HD Video to be played efficiently.")
                                .arg(vboxGlobal().formatSize(uNeedBytes, 0, FormatSize_RoundUp));
                return true;
            }
        }
#endif /* VBOX_WITH_VIDEOHWACCEL */
#if 0
# ifdef VBOX_WITH_CRHGSMI
        if (m_pCheckbox3D->isChecked() && m_fWddmModeSupported)
        {
            int cGuestScreenCount = m_pEditorVideoScreenCount->value();
            uNeedBytes += VBoxGlobal::required3DWddmOffscreenVideoMemory(m_guestOSType.GetId(), cGuestScreenCount);
            uNeedBytes = qMax(uNeedBytes, 128 * _1M);
            uNeedBytes = qMin(uNeedBytes, 256 * _1M);
            if ((quint64)m_pEditorVideoMemorySize->value() * _1M < uNeedBytes)
            {
                strWarning = tr("you have 3D Acceleration enabled for a operation system which uses the WDDM video driver. "
                                "For maximal performance set the guest VRAM to at least <b>%1</b>.")
                                .arg(vboxGlobal().formatSize(uNeedBytes, 0, FormatSize_RoundUp));
                return true;
            }
        }
# endif /* VBOX_WITH_CRHGSMI */
#endif /* 0 */
    }

    /* Check mode availability: */
#ifdef VBOX_WITH_VIDEOHWACCEL
    /* 2D video acceleration is available for Windows guests only: */
    if (m_pCheckbox2DVideo->isChecked() && !m_f2DVideoAccelerationSupported)
    {
        strWarning = tr("you have 2D Video Acceleration enabled. As 2D Video Acceleration "
                        "is supported for Windows guests only, this feature will be disabled.");
        return true;
    }
#endif /* VBOX_WITH_VIDEOHWACCEL */

    /* Check VRDE server port: */
    if (m_pEditorRemoteDisplayPort->text().trimmed().isEmpty())
    {
        strWarning = tr("server port value was not specified.");
        return false;
    }

    /* Check VRDE server timeout: */
    if (m_pEditorRemoteDisplayTimeout->text().trimmed().isEmpty())
    {
        strWarning = tr("authentication timeout value was not specified.");
        return false;
    }

    /* Pass by default: */
    return true;
}

void UIMachineSettingsDisplay::setOrderAfter(QWidget *pWidget)
{
    /* Video tab-order */
    setTabOrder(pWidget, m_pTabWidget->focusProxy());
    setTabOrder(m_pTabWidget->focusProxy(), m_pSliderVideoMemorySize);
    setTabOrder(m_pSliderVideoMemorySize, m_pEditorVideoMemorySize);
    setTabOrder(m_pEditorVideoMemorySize, m_pSliderVideoScreenCount);
    setTabOrder(m_pSliderVideoScreenCount, m_pEditorVideoScreenCount);
    setTabOrder(m_pEditorVideoScreenCount, m_pCheckbox3D);
#ifdef VBOX_WITH_VIDEOHWACCEL
    setTabOrder(m_pCheckbox3D, m_pCheckbox2DVideo);
    setTabOrder(m_pCheckbox2DVideo, m_pCheckboxRemoteDisplay);
#else /* VBOX_WITH_VIDEOHWACCEL */
    setTabOrder(m_pCheckbox3D, m_pCheckboxRemoteDisplay);
#endif /* !VBOX_WITH_VIDEOHWACCEL */

    /* Remote Display tab-order: */
    setTabOrder(m_pCheckboxRemoteDisplay, m_pEditorRemoteDisplayPort);
    setTabOrder(m_pEditorRemoteDisplayPort, m_pComboRemoteDisplayAuthMethod);
    setTabOrder(m_pComboRemoteDisplayAuthMethod, m_pEditorRemoteDisplayTimeout);
    setTabOrder(m_pEditorRemoteDisplayTimeout, m_pCheckboxMultipleConn);

    /* Video Capture tab-order: */
    setTabOrder(m_pCheckboxMultipleConn, m_pCheckboxVideoCapture);
    setTabOrder(m_pCheckboxVideoCapture, m_pEditorVideoCapturePath);
    setTabOrder(m_pEditorVideoCapturePath, m_pComboVideoCaptureSize);
    setTabOrder(m_pComboVideoCaptureSize, m_pEditorVideoCaptureWidth);
    setTabOrder(m_pEditorVideoCaptureWidth, m_pEditorVideoCaptureHeight);
    setTabOrder(m_pEditorVideoCaptureHeight, m_pSliderVideoCaptureFrameRate);
    setTabOrder(m_pSliderVideoCaptureFrameRate, m_pEditorVideoCaptureFrameRate);
    setTabOrder(m_pEditorVideoCaptureFrameRate, m_pSliderVideoCaptureQuality);
    setTabOrder(m_pSliderVideoCaptureQuality, m_pEditorVideoCaptureBitRate);
}

void UIMachineSettingsDisplay::retranslateUi()
{
    /* Translate uic generated strings */
    Ui::UIMachineSettingsDisplay::retranslateUi(this);

    /* Video stuff: */
    CSystemProperties sys = vboxGlobal().virtualBox().GetSystemProperties();
    m_pLabelVideoMemorySizeMin->setText(tr("<qt>%1&nbsp;MB</qt>").arg(m_iMinVRAM));
    m_pLabelVideoMemorySizeMax->setText(tr("<qt>%1&nbsp;MB</qt>").arg(m_iMaxVRAMVisible));
    m_pLabelVideoScreenCountMin->setText(tr("<qt>%1</qt>").arg(1));
    m_pLabelVideoScreenCountMax->setText(tr("<qt>%1</qt>").arg(qMin(sys.GetMaxGuestMonitors(), (ULONG)8)));

    /* Remote Display stuff: */
    m_pComboRemoteDisplayAuthMethod->setItemText(0, gpConverter->toString(KAuthType_Null));
    m_pComboRemoteDisplayAuthMethod->setItemText(1, gpConverter->toString(KAuthType_External));
    m_pComboRemoteDisplayAuthMethod->setItemText(2, gpConverter->toString(KAuthType_Guest));

    /* Video Capture stuff: */
    m_pComboVideoCaptureSize->setItemText(0, tr("User Defined"));
    m_pLabelVideoCaptureFrameRateMin->setText(tr("%1 fps").arg(m_pSliderVideoCaptureFrameRate->minimum()));
    m_pLabelVideoCaptureFrameRateMax->setText(tr("%1 fps").arg(m_pSliderVideoCaptureFrameRate->maximum()));
    m_pLabelVideoCaptureFrameRateUnits->setText(tr("fps"));
    m_pLabelVideoCaptureQualityMin->setText(tr("low", "quality"));
    m_pLabelVideoCaptureQualityMed->setText(tr("medium", "quality"));
    m_pLabelVideoCaptureQualityMax->setText(tr("high", "quality"));
    m_pLabelVideoCaptureBitRateUnits->setText(tr("kbps"));
}

void UIMachineSettingsDisplay::polishPage()
{
    /* Get system data from cache: */
    const UIDataSettingsMachineDisplay &displayData = m_cache.base();

    /* Video tab: */
    m_pContainerVideo->setEnabled(isMachineOffline());
#ifdef VBOX_WITH_VIDEOHWACCEL
    m_pCheckbox2DVideo->setEnabled(VBoxGlobal::isAcceleration2DVideoAvailable());
#endif /* VBOX_WITH_VIDEOHWACCEL */

    /* Remote Display tab: */
    m_pTabWidget->setTabEnabled(1, displayData.m_fRemoteDisplayServerSupported);
    m_pContainerRemoteDisplay->setEnabled(isMachineInValidMode());
    m_pContainerRemoteDisplayOptions->setEnabled(m_pCheckboxRemoteDisplay->isChecked());
    m_pLabelRemoteDisplayOptions->setEnabled(isMachineOffline() || isMachineSaved());
    m_pCheckboxMultipleConn->setEnabled(isMachineOffline() || isMachineSaved());

    /* Video Capture tab: */
    m_pContainerVideoCapture->setEnabled(isMachineInValidMode());
    sltHandleVideoCaptureCheckboxToggle();
}

void UIMachineSettingsDisplay::sltHandleVideoMemorySizeSliderChange()
{
    /* Apply proposed memory-size: */
    m_pEditorVideoMemorySize->blockSignals(true);
    m_pEditorVideoMemorySize->setValue(m_pSliderVideoMemorySize->value());
    m_pEditorVideoMemorySize->blockSignals(false);

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsDisplay::sltHandleVideoMemorySizeEditorChange()
{
    /* Apply proposed memory-size: */
    m_pSliderVideoMemorySize->blockSignals(true);
    m_pSliderVideoMemorySize->setValue(m_pEditorVideoMemorySize->value());
    m_pSliderVideoMemorySize->blockSignals(false);

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsDisplay::sltHandleVideoScreenCountSliderChange()
{
    /* Apply proposed screen-count: */
    m_pEditorVideoScreenCount->blockSignals(true);
    m_pEditorVideoScreenCount->setValue(m_pSliderVideoScreenCount->value());
    m_pEditorVideoScreenCount->blockSignals(false);

    /* Update Video RAM requirements: */
    checkVRAMRequirements();

    /* Update Video Capture tab screen count: */
    updateVideoCaptureScreenCount();

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsDisplay::sltHandleVideoScreenCountEditorChange()
{
    /* Apply proposed screen-count: */
    m_pSliderVideoScreenCount->blockSignals(true);
    m_pSliderVideoScreenCount->setValue(m_pEditorVideoScreenCount->value());
    m_pSliderVideoScreenCount->blockSignals(false);

    /* Update Video RAM requirements: */
    checkVRAMRequirements();

    /* Update Video Capture tab screen count: */
    updateVideoCaptureScreenCount();

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureCheckboxToggle()
{
    /* Video Capture options should be enabled only if:
     * 1. Machine is in 'offline' or 'saved' state and check-box is checked,
     * 2. Machine is in 'online' state, check-box is checked, and video recording is *disabled* currently. */
    bool fIsVideoCaptureOptionsEnabled = ((isMachineOffline() || isMachineSaved()) && m_pCheckboxVideoCapture->isChecked()) ||
                                         (isMachineOnline() && !m_cache.base().m_fVideoCaptureEnabled && m_pCheckboxVideoCapture->isChecked());

    /* Video Capture Screens option should be enabled only if:
     * Machine is in *any* valid state and check-box is checked. */
    bool fIsVideoCaptureScreenOptionEnabled = isMachineInValidMode() && m_pCheckboxVideoCapture->isChecked();

    m_pLabelVideoCapturePath->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pEditorVideoCapturePath->setEnabled(fIsVideoCaptureOptionsEnabled);

    m_pLabelVideoCaptureSize->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pComboVideoCaptureSize->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pEditorVideoCaptureWidth->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pEditorVideoCaptureHeight->setEnabled(fIsVideoCaptureOptionsEnabled);

    m_pLabelVideoCaptureFrameRate->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pContainerSliderVideoCaptureFrameRate->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pEditorVideoCaptureFrameRate->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pLabelVideoCaptureFrameRateUnits->setEnabled(fIsVideoCaptureOptionsEnabled);

    m_pLabelVideoCaptureRate->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pContainerSliderVideoCaptureQuality->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pEditorVideoCaptureBitRate->setEnabled(fIsVideoCaptureOptionsEnabled);
    m_pLabelVideoCaptureBitRateUnits->setEnabled(fIsVideoCaptureOptionsEnabled);

    m_pLabelVideoCaptureScreens->setEnabled(fIsVideoCaptureScreenOptionEnabled);
    m_pScrollerVideoCaptureScreens->setEnabled(fIsVideoCaptureScreenOptionEnabled);
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureFrameSizeComboboxChange()
{
    /* Get the proposed size: */
    int iCurrentIndex = m_pComboVideoCaptureSize->currentIndex();
    QSize videoCaptureSize = m_pComboVideoCaptureSize->itemData(iCurrentIndex).toSize();

    /* Make sure its valid: */
    if (!videoCaptureSize.isValid())
        return;

    /* Apply proposed size: */
    m_pEditorVideoCaptureWidth->setValue(videoCaptureSize.width());
    m_pEditorVideoCaptureHeight->setValue(videoCaptureSize.height());
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureFrameWidthEditorChange()
{
    /* Look for preset: */
    lookForCorrespondingSizePreset();
    /* Update quality and bit-rate: */
    sltHandleVideoCaptureQualitySliderChange();
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureFrameHeightEditorChange()
{
    /* Look for preset: */
    lookForCorrespondingSizePreset();
    /* Update quality and bit-rate: */
    sltHandleVideoCaptureQualitySliderChange();
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureFrameRateSliderChange()
{
    /* Apply proposed frame-rate: */
    m_pEditorVideoCaptureFrameRate->blockSignals(true);
    m_pEditorVideoCaptureFrameRate->setValue(m_pSliderVideoCaptureFrameRate->value());
    m_pEditorVideoCaptureFrameRate->blockSignals(false);
    /* Update quality and bit-rate: */
    sltHandleVideoCaptureQualitySliderChange();
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureFrameRateEditorChange()
{
    /* Apply proposed frame-rate: */
    m_pSliderVideoCaptureFrameRate->blockSignals(true);
    m_pSliderVideoCaptureFrameRate->setValue(m_pEditorVideoCaptureFrameRate->value());
    m_pSliderVideoCaptureFrameRate->blockSignals(false);
    /* Update quality and bit-rate: */
    sltHandleVideoCaptureQualitySliderChange();
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureQualitySliderChange()
{
    /* Calculate/apply proposed bit-rate: */
    m_pEditorVideoCaptureBitRate->blockSignals(true);
    m_pEditorVideoCaptureBitRate->setValue(calculateBitRate(m_pEditorVideoCaptureWidth->value(),
                                                            m_pEditorVideoCaptureHeight->value(),
                                                            m_pEditorVideoCaptureFrameRate->value(),
                                                            m_pSliderVideoCaptureQuality->value()));
    m_pEditorVideoCaptureBitRate->blockSignals(false);
}

void UIMachineSettingsDisplay::sltHandleVideoCaptureBitRateEditorChange()
{
    /* Calculate/apply proposed quality: */
    m_pSliderVideoCaptureQuality->blockSignals(true);
    m_pSliderVideoCaptureQuality->setValue(calculateQuality(m_pEditorVideoCaptureWidth->value(),
                                                            m_pEditorVideoCaptureHeight->value(),
                                                            m_pEditorVideoCaptureFrameRate->value(),
                                                            m_pEditorVideoCaptureBitRate->value()));
    m_pSliderVideoCaptureQuality->blockSignals(false);
}

void UIMachineSettingsDisplay::prepare()
{
    /* Apply UI decorations: */
    Ui::UIMachineSettingsDisplay::setupUi(this);

    /* Prepare tabs: */
    prepareVideoTab();
    prepareRemoteDisplayTab();
    prepareVideoCaptureTab();

    /* Prepare validation: */
    prepareValidation();

    /* Translate finally: */
    retranslateUi();
}

void UIMachineSettingsDisplay::prepareVideoTab()
{
    /* Prepare memory-size slider: */
    CSystemProperties sys = vboxGlobal().virtualBox().GetSystemProperties();
    m_iMinVRAM = sys.GetMinGuestVRAM();
    m_iMaxVRAM = sys.GetMaxGuestVRAM();
    m_iMaxVRAMVisible = m_iMaxVRAM;
    const uint cHostScreens = QApplication::desktop()->screenCount();
    m_pSliderVideoMemorySize->setMinimum(m_iMinVRAM);
    m_pSliderVideoMemorySize->setMaximum(m_iMaxVRAMVisible);
    m_pSliderVideoMemorySize->setPageStep(calcPageStep(m_iMaxVRAMVisible));
    m_pSliderVideoMemorySize->setSingleStep(m_pSliderVideoMemorySize->pageStep() / 4);
    m_pSliderVideoMemorySize->setTickInterval(m_pSliderVideoMemorySize->pageStep());
    m_pSliderVideoMemorySize->setSnappingEnabled(true);
    m_pSliderVideoMemorySize->setErrorHint(0, 1);
    connect(m_pSliderVideoMemorySize, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoMemorySizeSliderChange()));

    /* Prepare memory-size editor: */
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoMemorySize, 4);
    m_pEditorVideoMemorySize->setMinimum(m_iMinVRAM);
    m_pEditorVideoMemorySize->setMaximum(m_iMaxVRAMVisible);
    connect(m_pEditorVideoMemorySize, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoMemorySizeEditorChange()));

    /* Prepare screen-count slider: */
    const uint cMinGuestScreens = 1;
    const uint cMaxGuestScreens = sys.GetMaxGuestMonitors();
    const uint cMaxGuestScreensForSlider = qMin(cMaxGuestScreens, (uint)8);
    m_pSliderVideoScreenCount->setMinimum(cMinGuestScreens);
    m_pSliderVideoScreenCount->setMaximum(cMaxGuestScreensForSlider);
    m_pSliderVideoScreenCount->setPageStep(1);
    m_pSliderVideoScreenCount->setSingleStep(1);
    m_pSliderVideoScreenCount->setTickInterval(1);
    m_pSliderVideoScreenCount->setOptimalHint(cMinGuestScreens, cHostScreens);
    m_pSliderVideoScreenCount->setWarningHint(cHostScreens, cMaxGuestScreensForSlider);
    connect(m_pSliderVideoScreenCount, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoScreenCountSliderChange()));

    /* Prepare screen-count editor: */
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoScreenCount, 3);
    m_pEditorVideoScreenCount->setMinimum(1);
    m_pEditorVideoScreenCount->setMaximum(cMaxGuestScreens);
    connect(m_pEditorVideoScreenCount, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoScreenCountEditorChange()));

#ifndef VBOX_WITH_VIDEOHWACCEL
    /* Hide check-box if not supported: */
    m_pCheckbox2DVideo->setVisible(false);
#endif /* VBOX_WITH_VIDEOHWACCEL */
}

void UIMachineSettingsDisplay::prepareRemoteDisplayTab()
{
    /* Setup validators: */
    m_pEditorRemoteDisplayPort->setValidator(new QRegExpValidator(QRegExp("(([0-9]{1,5}(\\-[0-9]{1,5}){0,1}),)*([0-9]{1,5}(\\-[0-9]{1,5}){0,1})"), this));
    m_pEditorRemoteDisplayTimeout->setValidator(new QIntValidator(this));

    /* Prepare auth-method combo: */
    m_pComboRemoteDisplayAuthMethod->insertItem(0, ""); /* KAuthType_Null */
    m_pComboRemoteDisplayAuthMethod->insertItem(1, ""); /* KAuthType_External */
    m_pComboRemoteDisplayAuthMethod->insertItem(2, ""); /* KAuthType_Guest */
}

void UIMachineSettingsDisplay::prepareVideoCaptureTab()
{
    /* Prepare Video Capture checkbox: */
    connect(m_pCheckboxVideoCapture, SIGNAL(toggled(bool)), this, SLOT(sltHandleVideoCaptureCheckboxToggle()));

    /* Prepare filepath selector: */
    m_pEditorVideoCapturePath->setEditable(false);
    m_pEditorVideoCapturePath->setMode(VBoxFilePathSelectorWidget::Mode_File_Save);

    /* Prepare frame-size combo-box: */
    m_pComboVideoCaptureSize->addItem(""); /* User Defined */
    m_pComboVideoCaptureSize->addItem("320 x 200 (16:10)",   QSize(320, 200));
    m_pComboVideoCaptureSize->addItem("640 x 480 (4:3)",     QSize(640, 480));
    m_pComboVideoCaptureSize->addItem("720 x 400 (9:5)",     QSize(720, 400));
    m_pComboVideoCaptureSize->addItem("720 x 480 (3:2)",     QSize(720, 480));
    m_pComboVideoCaptureSize->addItem("800 x 600 (4:3)",     QSize(800, 600));
    m_pComboVideoCaptureSize->addItem("1024 x 768 (4:3)",    QSize(1024, 768));
    m_pComboVideoCaptureSize->addItem("1280 x 720 (16:9)",   QSize(1280, 720));
    m_pComboVideoCaptureSize->addItem("1280 x 800 (16:10)",  QSize(1280, 800));
    m_pComboVideoCaptureSize->addItem("1280 x 960 (4:3)",    QSize(1280, 960));
    m_pComboVideoCaptureSize->addItem("1280 x 1024 (5:4)",   QSize(1280, 1024));
    m_pComboVideoCaptureSize->addItem("1366 x 768 (16:9)",   QSize(1366, 768));
    m_pComboVideoCaptureSize->addItem("1440 x 900 (16:10)",  QSize(1440, 900));
    m_pComboVideoCaptureSize->addItem("1440 x 1080 (4:3)",   QSize(1440, 1080));
    m_pComboVideoCaptureSize->addItem("1600 x 900 (16:9)",   QSize(1600, 900));
    m_pComboVideoCaptureSize->addItem("1680 x 1050 (16:10)", QSize(1680, 1050));
    m_pComboVideoCaptureSize->addItem("1600 x 1200 (4:3)",   QSize(1600, 1200));
    m_pComboVideoCaptureSize->addItem("1920 x 1080 (16:9)",  QSize(1920, 1080));
    m_pComboVideoCaptureSize->addItem("1920 x 1200 (16:10)", QSize(1920, 1200));
    m_pComboVideoCaptureSize->addItem("1920 x 1440 (4:3)",   QSize(1920, 1440));
    connect(m_pComboVideoCaptureSize, SIGNAL(currentIndexChanged(int)), this, SLOT(sltHandleVideoCaptureFrameSizeComboboxChange()));

    /* Prepare frame-width/height editors: */
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoCaptureWidth, 5);
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoCaptureHeight, 5);
    m_pEditorVideoCaptureWidth->setMinimum(16);
    m_pEditorVideoCaptureWidth->setMaximum(1920);
    m_pEditorVideoCaptureHeight->setMinimum(16);
    m_pEditorVideoCaptureHeight->setMaximum(1440);
    connect(m_pEditorVideoCaptureWidth, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureFrameWidthEditorChange()));
    connect(m_pEditorVideoCaptureHeight, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureFrameHeightEditorChange()));

    /* Prepare frame-rate slider: */
    m_pSliderVideoCaptureFrameRate->setMinimum(1);
    m_pSliderVideoCaptureFrameRate->setMaximum(30);
    m_pSliderVideoCaptureFrameRate->setPageStep(1);
    m_pSliderVideoCaptureFrameRate->setSingleStep(1);
    m_pSliderVideoCaptureFrameRate->setTickInterval(1);
    m_pSliderVideoCaptureFrameRate->setSnappingEnabled(true);
    m_pSliderVideoCaptureFrameRate->setOptimalHint(1, 25);
    m_pSliderVideoCaptureFrameRate->setWarningHint(25, 30);
    connect(m_pSliderVideoCaptureFrameRate, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureFrameRateSliderChange()));

    /* Prepare frame-rate editor: */
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoCaptureFrameRate, 3);
    m_pEditorVideoCaptureFrameRate->setMinimum(1);
    m_pEditorVideoCaptureFrameRate->setMaximum(30);
    connect(m_pEditorVideoCaptureFrameRate, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureFrameRateEditorChange()));

    /* Prepare quality combo-box: */
    m_pContainerLayoutSliderVideoCaptureQuality->setColumnStretch(1, 4);
    m_pContainerLayoutSliderVideoCaptureQuality->setColumnStretch(3, 5);
    m_pSliderVideoCaptureQuality->setMinimum(1);
    m_pSliderVideoCaptureQuality->setMaximum(10);
    m_pSliderVideoCaptureQuality->setPageStep(1);
    m_pSliderVideoCaptureQuality->setSingleStep(1);
    m_pSliderVideoCaptureQuality->setTickInterval(1);
    m_pSliderVideoCaptureQuality->setSnappingEnabled(true);
    m_pSliderVideoCaptureQuality->setOptimalHint(1, 5);
    m_pSliderVideoCaptureQuality->setWarningHint(5, 9);
    m_pSliderVideoCaptureQuality->setErrorHint(9, 10);
    connect(m_pSliderVideoCaptureQuality, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureQualitySliderChange()));

    /* Prepare bit-rate editor: */
    vboxGlobal().setMinimumWidthAccordingSymbolCount(m_pEditorVideoCaptureBitRate, 5);
    m_pEditorVideoCaptureBitRate->setMinimum(32);
    m_pEditorVideoCaptureBitRate->setMaximum(2048);
    connect(m_pEditorVideoCaptureBitRate, SIGNAL(valueChanged(int)), this, SLOT(sltHandleVideoCaptureBitRateEditorChange()));
}

void UIMachineSettingsDisplay::prepareValidation()
{
    /* Configure validation: */
    connect(m_pCheckbox3D, SIGNAL(stateChanged(int)), this, SLOT(revalidate()));
#ifdef VBOX_WITH_VIDEOHWACCEL
    connect(m_pCheckbox2DVideo, SIGNAL(stateChanged(int)), this, SLOT(revalidate()));
#endif /* VBOX_WITH_VIDEOHWACCEL */
    connect(m_pCheckboxRemoteDisplay, SIGNAL(toggled(bool)), this, SLOT(revalidate()));
    connect(m_pEditorRemoteDisplayPort, SIGNAL(textChanged(const QString&)), this, SLOT(revalidate()));
    connect(m_pEditorRemoteDisplayTimeout, SIGNAL(textChanged(const QString&)), this, SLOT(revalidate()));
}

void UIMachineSettingsDisplay::checkVRAMRequirements()
{
    /* Make sure guest OS type is set: */
    if (m_guestOSType.isNull())
        return;

    /* Get monitors count and base video memory requirements: */
    int cGuestScreenCount = m_pEditorVideoScreenCount->value();
    quint64 uNeedMBytes = VBoxGlobal::requiredVideoMemory(m_guestOSType.GetId(), cGuestScreenCount) / _1M;

    /* Initial value: */
    m_iMaxVRAMVisible = cGuestScreenCount * 32;

    /* No more than m_iMaxVRAM: */
    if (m_iMaxVRAMVisible > m_iMaxVRAM)
        m_iMaxVRAMVisible = m_iMaxVRAM;

    /* No less than 128MB (if possible): */
    if (m_iMaxVRAMVisible < 128 && m_iMaxVRAM >= 128)
        m_iMaxVRAMVisible = 128;

    /* No less than initial VRAM size (wtf?): */
    if (m_iMaxVRAMVisible < m_iInitialVRAM)
        m_iMaxVRAMVisible = m_iInitialVRAM;

#ifdef VBOX_WITH_VIDEOHWACCEL
    if (m_pCheckbox2DVideo->isChecked() && m_f2DVideoAccelerationSupported)
    {
        uNeedMBytes += VBoxGlobal::required2DOffscreenVideoMemory() / _1M;
    }
#endif /* VBOX_WITH_VIDEOHWACCEL */

#ifdef VBOX_WITH_CRHGSMI
    if (m_pCheckbox3D->isChecked() && m_fWddmModeSupported)
    {
# if 0
        uNeedMBytes += VBoxGlobal::required3DWddmOffscreenVideoMemory(m_guestOSType.GetId(), cGuestScreenCount) / _1M;
        uNeedMBytes = qMax(uNeedMBytes, 128);
        uNeedMBytes = qMin(uNeedMBytes, 256);
# endif
        /* No less than 256MB (if possible): */
        if (m_iMaxVRAMVisible < 256 && m_iMaxVRAM >= 256)
            m_iMaxVRAMVisible = 256;
    }
#endif /* VBOX_WITH_CRHGSMI */

    m_pEditorVideoMemorySize->setMaximum(m_iMaxVRAMVisible);
    m_pSliderVideoMemorySize->setMaximum(m_iMaxVRAMVisible);
    m_pSliderVideoMemorySize->setPageStep(calcPageStep(m_iMaxVRAMVisible));
    m_pSliderVideoMemorySize->setWarningHint(1, qMin((int)uNeedMBytes, m_iMaxVRAMVisible));
    m_pSliderVideoMemorySize->setOptimalHint(qMin((int)uNeedMBytes, m_iMaxVRAMVisible), m_iMaxVRAMVisible);
    m_pLabelVideoMemorySizeMax->setText(tr("<qt>%1&nbsp;MB</qt>").arg(m_iMaxVRAMVisible));
}

bool UIMachineSettingsDisplay::shouldWeWarnAboutLowVideoMemory()
{
    bool fResult = true;

    QStringList excludingOSList = QStringList()
        << "Other" << "DOS" << "Netware" << "L4" << "QNX" << "JRockitVE";
    if (excludingOSList.contains(m_guestOSType.GetId()))
        fResult = false;

    return fResult;
}

/* static */
int UIMachineSettingsDisplay::calcPageStep(int iMax)
{
    /* Reasonable max. number of page steps is 32. */
    uint page = ((uint)iMax + 31) / 32;
    /* Make it a power of 2: */
    uint p = page, p2 = 0x1;
    while ((p >>= 1))
        p2 <<= 1;
    if (page != p2)
        p2 <<= 1;
    if (p2 < 4)
        p2 = 4;
    return (int)p2;
}

void UIMachineSettingsDisplay::lookForCorrespondingSizePreset()
{
    /* Look for video-capture size preset: */
    lookForCorrespondingPreset(m_pComboVideoCaptureSize,
                               QSize(m_pEditorVideoCaptureWidth->value(),
                                     m_pEditorVideoCaptureHeight->value()));
}

void UIMachineSettingsDisplay::updateVideoCaptureScreenCount()
{
    /* Update copy of the cached item to get the desired result: */
    QVector<BOOL> screens = m_cache.base().m_screens;
    screens.resize(m_pEditorVideoScreenCount->value());
    m_pScrollerVideoCaptureScreens->setValue(screens);
}

/* static */
void UIMachineSettingsDisplay::lookForCorrespondingPreset(QComboBox *pWhere, const QVariant &whichData)
{
    /* Use passed iterator to look for corresponding preset of passed combo-box: */
    int iLookupResult = pWhere->findData(whichData);
    if (iLookupResult != -1 && pWhere->currentIndex() != iLookupResult)
        pWhere->setCurrentIndex(iLookupResult);
    else if (iLookupResult == -1 && pWhere->currentIndex() != 0)
        pWhere->setCurrentIndex(0);
}

/* static */
int UIMachineSettingsDisplay::calculateBitRate(int iFrameWidth, int iFrameHeight, int iFrameRate, int iQuality)
{
    /* Linear quality<=>bit-rate scale-factor: */
    double dResult = (double)iQuality
                   * (double)iFrameWidth * (double)iFrameHeight * (double)iFrameRate
                   / (double)10 /* translate quality to [%] */
                   / (double)1024 /* translate bit-rate to [kbps] */
                   / (double)18.75 /* linear scale factor */;
    return (int)dResult;
}

/* static */
int UIMachineSettingsDisplay::calculateQuality(int iFrameWidth, int iFrameHeight, int iFrameRate, int iBitRate)
{
    /* Linear bit-rate<=>quality scale-factor: */
    double dResult = (double)iBitRate
                   / (double)iFrameWidth / (double)iFrameHeight / (double)iFrameRate
                   * (double)10 /* translate quality to [%] */
                   * (double)1024 /* translate bit-rate to [kbps] */
                   * (double)18.75 /* linear scale factor */;
    return (int)dResult;
}

