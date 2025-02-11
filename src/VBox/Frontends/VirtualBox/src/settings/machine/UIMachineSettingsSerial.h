/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIMachineSettingsSerial class declaration
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

#ifndef __UIMachineSettingsSerial_h__
#define __UIMachineSettingsSerial_h__

/* GUI includes: */
#include "UISettingsPage.h"
#include "UIMachineSettingsSerial.gen.h"

/* Forward declarations */
class UIMachineSettingsSerialPage;
class QITabWidget;

/* Machine settings / Serial page / Port data: */
struct UIDataSettingsMachineSerialPort
{
    /* Default constructor: */
    UIDataSettingsMachineSerialPort()
        : m_iSlot(-1)
        , m_fPortEnabled(false)
        , m_uIRQ(0)
        , m_uIOBase(0)
        , m_hostMode(KPortMode_Disconnected)
        , m_fServer(false)
        , m_strPath(QString()) {}
    /* Functions: */
    bool equal(const UIDataSettingsMachineSerialPort &other) const
    {
        return (m_iSlot == other.m_iSlot) &&
               (m_fPortEnabled == other.m_fPortEnabled) &&
               (m_uIRQ == other.m_uIRQ) &&
               (m_uIOBase == other.m_uIOBase) &&
               (m_hostMode == other.m_hostMode) &&
               (m_fServer == other.m_fServer) &&
               (m_strPath == other.m_strPath);
    }
    /* Operators: */
    bool operator==(const UIDataSettingsMachineSerialPort &other) const { return equal(other); }
    bool operator!=(const UIDataSettingsMachineSerialPort &other) const { return !equal(other); }
    /* Variables: */
    int m_iSlot;
    bool m_fPortEnabled;
    ulong m_uIRQ;
    ulong m_uIOBase;
    KPortMode m_hostMode;
    bool m_fServer;
    QString m_strPath;
};
typedef UISettingsCache<UIDataSettingsMachineSerialPort> UICacheSettingsMachineSerialPort;

/* Machine settings / Serial page / Ports data: */
struct UIDataSettingsMachineSerial
{
    /* Default constructor: */
    UIDataSettingsMachineSerial() {}
    /* Operators: */
    bool operator==(const UIDataSettingsMachineSerial& /* other */) const { return true; }
    bool operator!=(const UIDataSettingsMachineSerial& /* other */) const { return false; }
};
typedef UISettingsCachePool<UIDataSettingsMachineSerial, UICacheSettingsMachineSerialPort> UICacheSettingsMachineSerial;

class UIMachineSettingsSerial : public QIWithRetranslateUI<QWidget>,
                             public Ui::UIMachineSettingsSerial
{
    Q_OBJECT;

public:

    UIMachineSettingsSerial(UIMachineSettingsSerialPage *pParent);

    void polishTab();

    void fetchPortData(const UICacheSettingsMachineSerialPort &data);
    void uploadPortData(UICacheSettingsMachineSerialPort &data);

    QWidget* setOrderAfter (QWidget *aAfter);

    QString pageTitle() const;
    bool isUserDefined();

protected:

    void retranslateUi();

private slots:

    void mGbSerialToggled (bool aOn);
    void mCbNumberActivated (const QString &aText);
    void mCbModeActivated (const QString &aText);

private:

    /* Helper: Prepare stuff: */
    void prepareValidation();

    UIMachineSettingsSerialPage *m_pParent;
    int m_iSlot;
};

/* Machine settings / Serial page: */
class UIMachineSettingsSerialPage : public UISettingsPageMachine
{
    Q_OBJECT;

public:

    UIMachineSettingsSerialPage();

protected:

    /* Load data to cache from corresponding external object(s),
     * this task COULD be performed in other than GUI thread: */
    void loadToCacheFrom(QVariant &data);
    /* Load data to corresponding widgets from cache,
     * this task SHOULD be performed in GUI thread only: */
    void getFromCache();

    /* Save data from corresponding widgets to cache,
     * this task SHOULD be performed in GUI thread only: */
    void putToCache();
    /* Save data from cache to corresponding external object(s),
     * this task COULD be performed in other than GUI thread: */
    void saveFromCacheTo(QVariant &data);

    /* Page changed: */
    bool changed() const { return m_cache.wasChanged(); }

    /* API: Validation stuff: */
    bool validate(QString &strWarning, QString &strTitle);

    void retranslateUi();

private:

    void polishPage();

    QITabWidget *mTabWidget;

    /* Cache: */
    UICacheSettingsMachineSerial m_cache;
};

#endif // __UIMachineSettingsSerial_h__

