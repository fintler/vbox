/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VBoxGlobal class declaration
 */

/*
 * Copyright (C) 2006-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __VBoxGlobal_h__
#define __VBoxGlobal_h__

/* Qt includes: */
#include <QApplication>
#include <QLayout>
#include <QMenu>
#include <QStyle>
#include <QHash>
#include <QFileIconProvider>
#ifdef Q_WS_MAC
# include <QSet>
#endif /* Q_WS_MAC */

/* GUI includes: */
#include "UIDefs.h"
#include "VBoxGlobalSettings.h"
#include "UIMedium.h"

/* COM includes: */
#include "VBox/com/Guid.h"
#include "CHost.h"
#include "CVirtualBox.h"
#include "CSession.h"
#include "CGuestOSType.h"

/* Forward declarations: */
class QAction;
class QLabel;
class QToolButton;
class UIMachine;
class CMachine;
class CMedium;
class CUSBDevice;
class QSpinBox;

// VBoxGlobal class
////////////////////////////////////////////////////////////////////////////////

class UISelectorWindow;
class VBoxUpdateDlg;

class VBoxGlobal : public QObject
{
    Q_OBJECT

public:

    /* Static API: Create/destroy stuff: */
    static VBoxGlobal* instance();
    static void create();
    static void destroy();

    bool isValid() { return mValid; }

    static QString qtRTVersionString();
    static uint qtRTVersion();
    static QString qtCTVersionString();
    static uint qtCTVersion();

    QString vboxVersionString() const;
    QString vboxVersionStringNormalized() const;

    QString versionString() const { return mVerString; }
    bool isBeta() const;

    CVirtualBox virtualBox() const { return mVBox; }
    CHost host() const { return mHost; }

    VBoxGlobalSettings &settings() { return gset; }
    bool setSettings (VBoxGlobalSettings &gs);

    UISelectorWindow &selectorWnd();

    /* VM stuff: */
    bool startMachine(const QString &strMachineId);
    UIMachine* virtualMachine();
    QWidget* activeMachineWindow();

    bool is3DAvailableWorker() const;
    bool is3DAvailable() const { if (m3DAvailable < 0) return is3DAvailableWorker(); return m3DAvailable != 0; }

#ifdef VBOX_GUI_WITH_PIDFILE
    void createPidfile();
    void deletePidfile();
#endif

    /* branding */
    bool brandingIsActive (bool aForce = false);
    QString brandingGetKey (QString aKey);

    bool processArgs();

    bool switchToMachine(CMachine &machine);
    bool launchMachine(CMachine &machine, bool fHeadless = false);

    bool isVMConsoleProcess() const { return !vmUuid.isNull(); }
    bool showStartVMErrors() const { return mShowStartVMErrors; }
    QString managedVMUuid() const { return vmUuid; }
    QList<QUrl> &argUrlList() { return m_ArgUrlList; }

    RenderMode vmRenderMode() const { return vm_render_mode; }
    const char *vmRenderModeStr() const { return vm_render_mode_str; }
    bool isKWinManaged() const { return mIsKWinManaged; }

    const QRect availableGeometry(int iScreen = 0) const;

    bool isPatmDisabled() const { return mDisablePatm; }
    bool isCsamDisabled() const { return mDisableCsam; }
    bool isSupervisorCodeExecedRecompiled() const { return mRecompileSupervisor; }
    bool isUserCodeExecedRecompiled()       const { return mRecompileUser; }
    bool areWeToExecuteAllInIem()           const { return mExecuteAllInIem; }
    bool isDefaultWarpPct() const { return mWarpPct == 100; }
    uint32_t getWarpPct()       const { return mWarpPct; }

#ifdef VBOX_WITH_DEBUGGER_GUI
    bool isDebuggerEnabled(CMachine &aMachine);
    bool isDebuggerAutoShowEnabled(CMachine &aMachine);
    bool isDebuggerAutoShowCommandLineEnabled(CMachine &aMachine);
    bool isDebuggerAutoShowStatisticsEnabled(CMachine &aMachine);
    RTLDRMOD getDebuggerModule() const { return mhVBoxDbg; }

    bool isStartPausedEnabled() const { return mStartPaused; }
#else
    bool isDebuggerAutoShowEnabled(CMachine & /*aMachine*/) const { return false; }
    bool isDebuggerAutoShowCommandLineEnabled(CMachine & /*aMachine*/) const { return false; }
    bool isDebuggerAutoShowStatisticsEnabled(CMachine & /*aMachine*/) const { return false; }

    bool isStartPausedEnabled() const { return false; }
#endif

    /* VBox enum to/from string/icon/color convertors */

    QList <CGuestOSType> vmGuestOSFamilyList() const;
    QList <CGuestOSType> vmGuestOSTypeList (const QString &aFamilyId) const;
    QPixmap vmGuestOSTypeIcon (const QString &aTypeId) const;
    CGuestOSType vmGuestOSType (const QString &aTypeId,
                                const QString &aFamilyId = QString::null) const;
    QString vmGuestOSTypeDescription (const QString &aTypeId) const;

    static inline QString yearsToString (uint32_t cVal)
    {
        return tr("%n year(s)", "", cVal);
    }

    static inline QString monthsToString (uint32_t cVal)
    {
        return tr("%n month(s)", "", cVal);
    }

    static inline QString daysToString (uint32_t cVal)
    {
        return tr("%n day(s)", "", cVal);
    }

    static inline QString hoursToString (uint32_t cVal)
    {
        return tr("%n hour(s)", "", cVal);
    }

    static inline QString minutesToString (uint32_t cVal)
    {
        return tr("%n minute(s)", "", cVal);
    }

    static inline QString secondsToString (uint32_t cVal)
    {
        return tr("%n second(s)", "", cVal);
    }

    QString differencingMediumTypeName() const { return mDiskTypes_Differencing; }

    /**
     * Similar to toString (KMediumType), but returns 'Differencing' for
     * normal hard disks that have a parent.
     */
    QString mediumTypeString(const CMedium &medium) const;

    QStringList COMPortNames() const;
    QString toCOMPortName (ulong aIRQ, ulong aIOBase) const;
    bool toCOMPortNumbers (const QString &aName, ulong &aIRQ, ulong &aIOBase) const;

    QStringList LPTPortNames() const;
    QString toLPTPortName (ulong aIRQ, ulong aIOBase) const;
    bool toLPTPortNumbers (const QString &aName, ulong &aIRQ, ulong &aIOBase) const;

    QPixmap snapshotIcon (bool online) const
    {
        return online ? mOnlineSnapshotIcon : mOfflineSnapshotIcon;
    }

    static bool hasAllowedExtension(const QString &strExt, const QStringList &extList)
    {
        for (int i = 0; i < extList.size(); ++i)
            if (strExt.endsWith(extList.at(i), Qt::CaseInsensitive))
                return true;
        return false;
    }

    QIcon icon(QFileIconProvider::IconType type) { return m_globalIconProvider.icon(type); }
    QIcon icon(const QFileInfo &info) { return m_globalIconProvider.icon(info); }

    QPixmap warningIcon() const { return mWarningIcon; }
    QPixmap errorIcon() const { return mErrorIcon; }

    /* details generators */

    QString details (const CMedium &aHD, bool aPredictDiff, bool fUseHtml = true);

    QString details (const CUSBDevice &aDevice) const;
    QString toolTip (const CUSBDevice &aDevice) const;
    QString toolTip (const CUSBDeviceFilter &aFilter) const;

    QString detailsReport (const CMachine &aMachine, bool aWithLinks);

    /* VirtualBox helpers */

    CSession openSession(const QString &aId, KLockType aLockType = KLockType_Write);

    /** Shortcut to openSession (aId, true). */
    CSession openExistingSession(const QString &aId) { return openSession(aId, KLockType_Shared); }

    void startEnumeratingMedia();

    void reloadProxySettings();

    /**
     * Returns a list of all currently registered media. This list is used to
     * globally track the accessibility state of all media on a dedicated thread.
     *
     * Note that the media list is initially empty (i.e. before the enumeration
     * process is started for the first time using #startEnumeratingMedia()).
     * See #startEnumeratingMedia() for more information about how meida are
     * sorted in the returned list.
     */
    const VBoxMediaList &currentMediaList() const { return mMediaList; }

    /** Returns true if the media enumeration is in progress. */
    bool isMediaEnumerationStarted() const { return mMediaEnumThread != NULL; }

    void addMedium (const UIMedium &);
    void updateMedium (const UIMedium &);
    void removeMedium (UIMediumType, const QString &);

    bool findMedium (const CMedium &, UIMedium &) const;
    UIMedium findMedium (const QString &aMediumId) const;

    /** Compact version of #findMediumTo(). Asserts if not found. */
    UIMedium getMedium (const CMedium &aObj) const
    {
        UIMedium medium;
        if (!findMedium (aObj, medium))
            AssertFailed();
        return medium;
    }

    QString openMediumWithFileOpenDialog(UIMediumType mediumType, QWidget *pParent = 0,
                                         const QString &strDefaultFolder = QString(), bool fUseLastFolder = true);
    QString openMedium(UIMediumType mediumType, QString strMediumLocation, QWidget *pParent = 0);

    /* Returns the number of current running Fe/Qt4 main windows. */
    int mainWindowCount();

    /* various helpers */

    QString languageName() const;
    QString languageCountry() const;
    QString languageNameEnglish() const;
    QString languageCountryEnglish() const;
    QString languageTranslators() const;

    void retranslateUi();

    /* public static stuff */

    static bool isDOSType (const QString &aOSTypeId);

    static QString languageId();
    static void loadLanguage (const QString &aLangId = QString::null);
    QString helpFile() const;

    static void setTextLabel (QToolButton *aToolButton, const QString &aTextLabel);

    static QRect normalizeGeometry (const QRect &aRectangle, const QRegion &aBoundRegion,
                                    bool aCanResize = true);
    static QRect getNormalized (const QRect &aRectangle, const QRegion &aBoundRegion,
                                bool aCanResize = true);
    static QRegion flip (const QRegion &aRegion);

    static void centerWidget (QWidget *aWidget, QWidget *aRelative,
                              bool aCanResize = true);

    static QChar decimalSep();
    static QString sizeRegexp();

    static quint64 parseSize (const QString &);
    static QString formatSize (quint64 aSize, uint aDecimal = 2, FormatSize aMode = FormatSize_Round);

    static quint64 requiredVideoMemory(const QString &strGuestOSTypeId, int cMonitors = 1);

    static QString locationForHTML (const QString &aFileName);

    static QString highlight (const QString &aStr, bool aToolTip = false);

    static QString replaceHtmlEntities(QString strText);
    static QString emphasize (const QString &aStr);

    static QString systemLanguageId();

    static bool activateWindow (WId aWId, bool aSwitchDesktop = true);

    static QString removeAccelMark (const QString &aText);

    static QString insertKeyToActionText (const QString &aText, const QString &aKey);
    static QString extractKeyFromActionText (const QString &aText);

    static QPixmap joinPixmaps (const QPixmap &aPM1, const QPixmap &aPM2);

    static QWidget *findWidget (QWidget *aParent, const char *aName,
                                const char *aClassName = NULL,
                                bool aRecursive = false);

    static QList <QPair <QString, QString> > MediumBackends(KDeviceType enmDeviceType);
    static QList <QPair <QString, QString> > HDDBackends();
    static QList <QPair <QString, QString> > DVDBackends();
    static QList <QPair <QString, QString> > FloppyBackends();

    static QString documentsPath();

#ifdef VBOX_WITH_VIDEOHWACCEL
    static bool isAcceleration2DVideoAvailable();

    /** additional video memory required for the best 2D support performance
     *  total amount of VRAM required is thus calculated as requiredVideoMemory + required2DOffscreenVideoMemory  */
    static quint64 required2DOffscreenVideoMemory();
#endif

#ifdef VBOX_WITH_CRHGSMI
    static bool isWddmCompatibleOsType(const QString &strGuestOSTypeId);
    static quint64 required3DWddmOffscreenVideoMemory(const QString &strGuestOSTypeId, int cMonitors = 1);
#endif /* VBOX_WITH_CRHGSMI */

    /* Returns full medium-format name for the given base medium-format name: */
    static QString fullMediumFormatName(const QString &strBaseMediumFormatName);

    /* Extra-data settings stuff: */
    static bool isApprovedByExtraData(CVirtualBox &vbox, const QString &strExtraDataKey);
    static bool isApprovedByExtraData(CMachine &machine, const QString &strExtraDataKey);
    static bool shouldWeAllowApplicationUpdate(CVirtualBox &vbox);
    static bool shouldWeShowMachine(CMachine &machine);
    static bool shouldWeAllowMachineReconfiguration(CMachine &machine,
                                                    bool fIncludingMachineGeneralCheck = false,
                                                    bool fIncludingMachineStateCheck = false);
    static bool shouldWeShowDetails(CMachine &machine,
                                    bool fIncludingMachineGeneralCheck = false);
    static bool shouldWeAutoMountGuestScreens(CMachine &machine, bool fIncludingSanityCheck = true);
    static bool shouldWeAllowSnapshotOperations(CMachine &machine, bool fIncludingSanityCheck = true);
    static RuntimeMenuType restrictedRuntimeMenuTypes(CMachine &machine);
    static UIVisualStateType restrictedVisualStateTypes(CMachine &machine);
    static QList<IndicatorType> restrictedStatusBarIndicators(CMachine &machine);
    static QList<MachineCloseAction> restrictedMachineCloseActions(CMachine &machine);
    static QList<GlobalSettingsPageType> restrictedGlobalSettingsPages(CVirtualBox &vbox);
    static QList<MachineSettingsPageType> restrictedMachineSettingsPages(CMachine &machine);

#ifdef RT_OS_LINUX
    static void checkForWrongUSBMounted();
#endif /* RT_OS_LINUX */

    /* Shame on Qt it hasn't stuff for tuning
     * widget size suitable for reflecting content of desired size.
     * For example QLineEdit, QSpinBox and similar widgets should have a methods
     * to strict the minimum width to reflect at least [n] symbols. */
    static void setMinimumWidthAccordingSymbolCount(QSpinBox *pSpinBox, int cCount);

signals:

    /**
     * Emitted at the beginning of the enumeration process started by
     * #startEnumeratingMedia().
     */
    void mediumEnumStarted();

    /**
     * Emitted when a new medium item from the list has updated its
     * accessibility state.
     */
    void mediumEnumerated (const UIMedium &aMedum);

    /**
     * Emitted at the end of the enumeration process started by
     * #startEnumeratingMedia(). The @a aList argument is passed for
     * convenience, it is exactly the same as returned by #currentMediaList().
     */
    void mediumEnumFinished (const VBoxMediaList &aList);

    /** Emitted when a new media is added using #addMedia(). */
    void mediumAdded (const UIMedium &);

    /** Emitted when the media is updated using #updateMedia(). */
    void mediumUpdated (const UIMedium &);

    /** Emitted when the media is removed using #removeMedia(). */
    void mediumRemoved (UIMediumType, const QString &);

public slots:

    bool openURL (const QString &aURL);

    void sltGUILanguageChange(QString strLang);
    void sltProcessGlobalSettingChange();

protected slots:

    /* Handlers: Prepare/cleanup stuff: */
    void prepare();
    void cleanup();

protected:

    bool event (QEvent *e);
    bool eventFilter (QObject *, QEvent *);

private:

    /* Constructor/destructor: */
    VBoxGlobal();
    ~VBoxGlobal();

#ifdef VBOX_WITH_DEBUGGER_GUI
    void initDebuggerVar(int *piDbgCfgVar, const char *pszEnvVar, const char *pszExtraDataName, bool fDefault = false);
    void setDebuggerVar(int *piDbgCfgVar, bool fState);
    bool isDebuggerWorker(int *piDbgCfgVar, CMachine &rMachine, const char *pszExtraDataName);
#endif

    bool mValid;

    CVirtualBox mVBox;
    CHost mHost;

    VBoxGlobalSettings gset;

    UISelectorWindow *mSelectorWnd;
    UIMachine *m_pVirtualMachine;

    QString vmUuid;
    QList<QUrl> m_ArgUrlList;

    /** Whether to show error message boxes for VM start errors. */
    bool mShowStartVMErrors;

    QThread *mMediaEnumThread;
    VBoxMediaList mMediaList;

    RenderMode vm_render_mode;
    const char * vm_render_mode_str;
    bool mIsKWinManaged;

    /** The --disable-patm option. */
    bool mDisablePatm;
    /** The --disable-csam option. */
    bool mDisableCsam;
    /** The --recompile-supervisor option. */
    bool mRecompileSupervisor;
    /** The --recompile-user option. */
    bool mRecompileUser;
    /** The --execute-all-in-iem option. */
    bool mExecuteAllInIem;
    /** The --warp-factor option value. */
    uint32_t mWarpPct;

#ifdef VBOX_WITH_DEBUGGER_GUI
    /** Whether the debugger should be accessible or not.
     * Use --dbg, the env.var. VBOX_GUI_DBG_ENABLED, --debug or the env.var.
     * VBOX_GUI_DBG_AUTO_SHOW to enable. */
    int mDbgEnabled;
    /** Whether to show the debugger automatically with the console.
     * Use --debug or the env.var. VBOX_GUI_DBG_AUTO_SHOW to enable. */
    int mDbgAutoShow;
    /** Whether to show the command line window when mDbgAutoShow is set. */
    int mDbgAutoShowCommandLine;
    /** Whether to show the statistics window when mDbgAutoShow is set. */
    int mDbgAutoShowStatistics;
    /** VBoxDbg module handle. */
    RTLDRMOD mhVBoxDbg;

    /** Whether to start the VM in paused state or not. */
    bool mStartPaused;
#endif

#if defined (Q_WS_WIN32)
    DWORD dwHTMLHelpCookie;
#endif

    QString mVerString;
    QString mBrandingConfig;

    int m3DAvailable;

    QList <QString> mFamilyIDs;
    QList <QList <CGuestOSType> > mTypes;
    QHash <QString, QPixmap *> mOsTypeIcons;

    QPixmap mOfflineSnapshotIcon, mOnlineSnapshotIcon;

    QString mDiskTypes_Differencing;

    QString mUserDefinedPortName;

    QPixmap mWarningIcon, mErrorIcon;

    QFileIconProvider m_globalIconProvider;

#ifdef VBOX_GUI_WITH_PIDFILE
    QString m_strPidfile;
#endif

    char mSettingsPw[256];
    bool mSettingsPwSet;

    /* API: Instance stuff: */
    static bool m_sfCleanupInProgress;
    static VBoxGlobal* m_spInstance;
    friend VBoxGlobal& vboxGlobal();
};

/* Shortcut to the static VBoxGlobal::instance() method: */
inline VBoxGlobal& vboxGlobal() { return *VBoxGlobal::instance(); }

#endif /* __VBoxGlobal_h__ */

