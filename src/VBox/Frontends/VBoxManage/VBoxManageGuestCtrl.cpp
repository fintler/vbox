/* $Id$ */
/** @file
 * VBoxManage - Implementation of guestcontrol command.
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "VBoxManage.h"

#ifndef VBOX_ONLY_DOCS

#include <VBox/com/array.h>
#include <VBox/com/com.h>
#include <VBox/com/ErrorInfo.h>
#include <VBox/com/errorprint.h>
#include <VBox/com/listeners.h>
#include <VBox/com/NativeEventQueue.h>
#include <VBox/com/string.h>
#include <VBox/com/VirtualBox.h>

#include <VBox/err.h>
#include <VBox/log.h>

#include <iprt/asm.h>
#include <iprt/dir.h>
#include <iprt/file.h>
#include <iprt/isofs.h>
#include <iprt/getopt.h>
#include <iprt/list.h>
#include <iprt/path.h>
#include <iprt/process.h> /* For RTProcSelf(). */
#include <iprt/thread.h>

#include <map>
#include <vector>

#ifdef USE_XPCOM_QUEUE
# include <sys/select.h>
# include <errno.h>
#endif

#include <signal.h>

#ifdef RT_OS_DARWIN
# include <CoreFoundation/CFRunLoop.h>
#endif

using namespace com;

/**
 *  Handler for guest events.
 */
class GuestEventListener
{
public:
    GuestEventListener(void)
    {
    }

    virtual ~GuestEventListener(void)
    {
    }

    HRESULT init(void)
    {
        return S_OK;
    }

    void uninit(void)
    {
    }

    STDMETHOD(HandleEvent)(VBoxEventType_T aType, IEvent *aEvent)
    {
        switch (aType)
        {
            case VBoxEventType_OnGuestSessionRegistered:
            {
                ComPtr<IGuestSessionRegisteredEvent> pEvent = aEvent;
                Assert(!pEvent.isNull());

                ComPtr<IGuestSession> pSession;
                HRESULT rc = pEvent->COMGETTER(Session)(pSession.asOutParam());
                AssertComRCBreakRC(rc);
                AssertBreak(!pSession.isNull());

                BOOL fRegistered;
                rc = pEvent->COMGETTER(Registered)(&fRegistered);
                AssertComRCBreakRC(rc);

                Bstr strName;
                rc = pSession->COMGETTER(Name)(strName.asOutParam());
                AssertComRCBreakRC(rc);
                ULONG uID;
                rc = pSession->COMGETTER(Id)(&uID);
                AssertComRCBreakRC(rc);

                RTPrintf("Session ID=%RU32 \"%s\" %s\n",
                         uID, Utf8Str(strName).c_str(),
                         fRegistered ? "registered" : "unregistered");

                pSession.setNull();
                break;
            }

            default:
                AssertFailed();
        }

        return S_OK;
    }
};
typedef ListenerImpl<GuestEventListener> GuestEventListenerImpl;
VBOX_LISTENER_DECLARE(GuestEventListenerImpl)

/** Set by the signal handler. */
static volatile bool         g_fGuestCtrlCanceled = false;
/** Our global session object which is also used in the
 *  signal handler to abort operations properly. */
static ComPtr<IGuestSession> g_pGuestSession;

typedef struct COPYCONTEXT
{
    COPYCONTEXT() : fVerbose(false), fDryRun(false), fHostToGuest(false)
    {
    }

    ComPtr<IGuestSession> pGuestSession;
    bool fVerbose;
    bool fDryRun;
    bool fHostToGuest;
} COPYCONTEXT, *PCOPYCONTEXT;

/**
 * An entry for a source element, including an optional DOS-like wildcard (*,?).
 */
class SOURCEFILEENTRY
{
    public:

        SOURCEFILEENTRY(const char *pszSource, const char *pszFilter)
                        : mSource(pszSource),
                          mFilter(pszFilter) {}

        SOURCEFILEENTRY(const char *pszSource)
                        : mSource(pszSource)
        {
            Parse(pszSource);
        }

        const char* GetSource() const
        {
            return mSource.c_str();
        }

        const char* GetFilter() const
        {
            return mFilter.c_str();
        }

    private:

        int Parse(const char *pszPath)
        {
            AssertPtrReturn(pszPath, VERR_INVALID_POINTER);

            if (   !RTFileExists(pszPath)
                && !RTDirExists(pszPath))
            {
                /* No file and no directory -- maybe a filter? */
                char *pszFilename = RTPathFilename(pszPath);
                if (   pszFilename
                    && strpbrk(pszFilename, "*?"))
                {
                    /* Yep, get the actual filter part. */
                    mFilter = RTPathFilename(pszPath);
                    /* Remove the filter from actual sourcec directory name. */
                    RTPathStripFilename(mSource.mutableRaw());
                    mSource.jolt();
                }
            }

            return VINF_SUCCESS; /* @todo */
        }

    private:

        Utf8Str mSource;
        Utf8Str mFilter;
};
typedef std::vector<SOURCEFILEENTRY> SOURCEVEC, *PSOURCEVEC;

/**
 * An entry for an element which needs to be copied/created to/on the guest.
 */
typedef struct DESTFILEENTRY
{
    DESTFILEENTRY(Utf8Str strFileName) : mFileName(strFileName) {}
    Utf8Str mFileName;
} DESTFILEENTRY, *PDESTFILEENTRY;
/*
 * Map for holding destination entires, whereas the key is the destination
 * directory and the mapped value is a vector holding all elements for this directoy.
 */
typedef std::map< Utf8Str, std::vector<DESTFILEENTRY> > DESTDIRMAP, *PDESTDIRMAP;
typedef std::map< Utf8Str, std::vector<DESTFILEENTRY> >::iterator DESTDIRMAPITER, *PDESTDIRMAPITER;

/**
 * Special exit codes for returning errors/information of a
 * started guest process to the command line VBoxManage was started from.
 * Useful for e.g. scripting.
 *
 * @note    These are frozen as of 4.1.0.
 */
enum EXITCODEEXEC
{
    EXITCODEEXEC_SUCCESS        = RTEXITCODE_SUCCESS,
    /* Process exited normally but with an exit code <> 0. */
    EXITCODEEXEC_CODE           = 16,
    EXITCODEEXEC_FAILED         = 17,
    EXITCODEEXEC_TERM_SIGNAL    = 18,
    EXITCODEEXEC_TERM_ABEND     = 19,
    EXITCODEEXEC_TIMEOUT        = 20,
    EXITCODEEXEC_DOWN           = 21,
    EXITCODEEXEC_CANCELED       = 22
};

/**
 * RTGetOpt-IDs for the guest execution control command line.
 */
enum GETOPTDEF_EXEC
{
    GETOPTDEF_EXEC_IGNOREORPHANEDPROCESSES = 1000,
    GETOPTDEF_EXEC_NO_PROFILE,
    GETOPTDEF_EXEC_OUTPUTFORMAT,
    GETOPTDEF_EXEC_DOS2UNIX,
    GETOPTDEF_EXEC_UNIX2DOS,
    GETOPTDEF_EXEC_PASSWORD,
    GETOPTDEF_EXEC_WAITFOREXIT,
    GETOPTDEF_EXEC_WAITFORSTDOUT,
    GETOPTDEF_EXEC_WAITFORSTDERR
};

enum GETOPTDEF_COPY
{
    GETOPTDEF_COPY_DRYRUN = 1000,
    GETOPTDEF_COPY_FOLLOW,
    GETOPTDEF_COPY_PASSWORD,
    GETOPTDEF_COPY_TARGETDIR
};

enum GETOPTDEF_MKDIR
{
    GETOPTDEF_MKDIR_PASSWORD = 1000
};

enum GETOPTDEF_SESSIONCLOSE
{
    GETOPTDEF_SESSIONCLOSE_ALL = 1000
};

enum GETOPTDEF_STAT
{
    GETOPTDEF_STAT_PASSWORD = 1000
};

enum OUTPUTTYPE
{
    OUTPUTTYPE_UNDEFINED = 0,
    OUTPUTTYPE_DOS2UNIX  = 10,
    OUTPUTTYPE_UNIX2DOS  = 20
};

static int ctrlCopyDirExists(PCOPYCONTEXT pContext, bool bGuest, const char *pszDir, bool *fExists);

#endif /* VBOX_ONLY_DOCS */

void usageGuestControl(PRTSTREAM pStrm, const char *pcszSep1, const char *pcszSep2)
{
    RTStrmPrintf(pStrm,
                       "%s guestcontrol %s    <uuid|vmname>\n"
                 "                            exec[ute]\n"
                 "                            --image <path to program> --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--domain <domain>] [--verbose] [--timeout <msec>]\n"
                 "                            [--environment \"<NAME>=<VALUE> [<NAME>=<VALUE>]\"]\n"
                 "                            [--wait-exit] [--wait-stdout] [--wait-stderr]\n"
                 "                            [--dos2unix] [--unix2dos]\n"
                 "                            [-- [<argument1>] ... [<argumentN>]]\n"
                 /** @todo Add a "--" parameter (has to be last parameter) to directly execute
                  *        stuff, e.g. "VBoxManage guestcontrol execute <VMName> --username <> ... -- /bin/rm -Rf /foo". */
                 "\n"
                 "                            copyfrom\n"
                 "                            <guest source> <host dest> --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--domain <domain>] [--verbose]\n"
                 "                            [--dryrun] [--follow] [--recursive]\n"
                 "\n"
                 "                            copyto|cp\n"
                 "                            <host source> <guest dest> --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--domain <domain>] [--verbose]\n"
                 "                            [--dryrun] [--follow] [--recursive]\n"
                 "\n"
                 "                            createdir[ectory]|mkdir|md\n"
                 "                            <guest directory>... --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--domain <domain>] [--verbose]\n"
                 "                            [--parents] [--mode <mode>]\n"
                 "\n"
                 "                            createtemp[orary]|mktemp\n"
                 "                            <template> --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--directory] [--secure] [--tmpdir <directory>]\n"
                 "                            [--domain <domain>] [--mode <mode>] [--verbose]\n"
                 "\n"
                 "                            list <all|sessions|processes> [--verbose]\n"
                 "\n"
                 /** @todo Add an own help group for "session" and "process" sub commands. */
                 "                            process kill --session-id <ID>\n"
                 "                                         | --session-name <name or pattern>\n"
                 "                                         [--verbose]\n"
                 "                                         <PID> ... <PID n>\n"
                 "\n"
                 "                            [p[s]]kill --session-id <ID>\n"
                 "                                       | --session-name <name or pattern>\n"
                 "                                       [--verbose]\n"
                 "                                       <PID> ... <PID n>\n"
                 "\n"
                 "                            session close  --session-id <ID>\n"
                 "                                         | --session-name <name or pattern>\n"
                 "                                         | --all\n"
                 "                                         [--verbose]\n"
                 "                            stat\n"
                 "                            <file>... --username <name>\n"
                 "                            [--passwordfile <file> | --password <password>]\n"
                 "                            [--domain <domain>] [--verbose]\n"
                 "\n"
                 "                            updateadditions\n"
                 "                            [--source <guest additions .ISO>] [--verbose]\n"
                 "                            [--wait-start]\n"
                 "                            [-- [<argument1>] ... [<argumentN>]]\n"
                 "\n"
                 "                            watch [--verbose]\n"
                 "\n", pcszSep1, pcszSep2);
}

#ifndef VBOX_ONLY_DOCS

#ifdef RT_OS_WINDOWS
static BOOL WINAPI guestCtrlSignalHandler(DWORD dwCtrlType)
{
    bool fEventHandled = FALSE;
    switch (dwCtrlType)
    {
        /* User pressed CTRL+C or CTRL+BREAK or an external event was sent
         * via GenerateConsoleCtrlEvent(). */
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_C_EVENT:
            ASMAtomicWriteBool(&g_fGuestCtrlCanceled, true);
            if (!g_pGuestSession.isNull())
                g_pGuestSession->Close();
            fEventHandled = TRUE;
            break;
        default:
            break;
        /** @todo Add other events here. */
    }

    return fEventHandled;
}
#else /* !RT_OS_WINDOWS */
/**
 * Signal handler that sets g_fGuestCtrlCanceled.
 *
 * This can be executed on any thread in the process, on Windows it may even be
 * a thread dedicated to delivering this signal.  Do not doing anything
 * unnecessary here.
 */
static void guestCtrlSignalHandler(int iSignal)
{
    NOREF(iSignal);
    ASMAtomicWriteBool(&g_fGuestCtrlCanceled, true);
    if (!g_pGuestSession.isNull())
        g_pGuestSession->Close();
}
#endif

/**
 * Installs a custom signal handler to get notified
 * whenever the user wants to intercept the program.
 *
 ** @todo Make this handler available for all VBoxManage modules?
 */
static int ctrlSignalHandlerInstall(void)
{
    int rc = VINF_SUCCESS;
#ifdef RT_OS_WINDOWS
    if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)guestCtrlSignalHandler, TRUE /* Add handler */))
    {
        rc = RTErrConvertFromWin32(GetLastError());
        RTMsgError("Unable to install console control handler, rc=%Rrc\n", rc);
    }
#else
    signal(SIGINT,   guestCtrlSignalHandler);
# ifdef SIGBREAK
    signal(SIGBREAK, guestCtrlSignalHandler);
# endif
#endif
    return rc;
}

/**
 * Uninstalls a previously installed signal handler.
 */
static int ctrlSignalHandlerUninstall(void)
{
    int rc = VINF_SUCCESS;
#ifdef RT_OS_WINDOWS
    if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)NULL, FALSE /* Remove handler */))
    {
        rc = RTErrConvertFromWin32(GetLastError());
        RTMsgError("Unable to uninstall console control handler, rc=%Rrc\n", rc);
    }
#else
    signal(SIGINT,   SIG_DFL);
# ifdef SIGBREAK
    signal(SIGBREAK, SIG_DFL);
# endif
#endif
    return rc;
}

/**
 * Translates a process status to a human readable
 * string.
 */
static const char *ctrlExecProcessStatusToText(ProcessStatus_T enmStatus)
{
    switch (enmStatus)
    {
        case ProcessStatus_Starting:
            return "starting";
        case ProcessStatus_Started:
            return "started";
        case ProcessStatus_Paused:
            return "paused";
        case ProcessStatus_Terminating:
            return "terminating";
        case ProcessStatus_TerminatedNormally:
            return "successfully terminated";
        case ProcessStatus_TerminatedSignal:
            return "terminated by signal";
        case ProcessStatus_TerminatedAbnormally:
            return "abnormally aborted";
        case ProcessStatus_TimedOutKilled:
            return "timed out";
        case ProcessStatus_TimedOutAbnormally:
            return "timed out, hanging";
        case ProcessStatus_Down:
            return "killed";
        case ProcessStatus_Error:
            return "error";
        default:
            break;
    }
    return "unknown";
}

static int ctrlExecProcessStatusToExitCode(ProcessStatus_T enmStatus, ULONG uExitCode)
{
    int vrc = EXITCODEEXEC_SUCCESS;
    switch (enmStatus)
    {
        case ProcessStatus_Starting:
            vrc = EXITCODEEXEC_SUCCESS;
            break;
        case ProcessStatus_Started:
            vrc = EXITCODEEXEC_SUCCESS;
            break;
        case ProcessStatus_Paused:
            vrc = EXITCODEEXEC_SUCCESS;
            break;
        case ProcessStatus_Terminating:
            vrc = EXITCODEEXEC_SUCCESS;
            break;
        case ProcessStatus_TerminatedNormally:
            vrc = !uExitCode ? EXITCODEEXEC_SUCCESS : EXITCODEEXEC_CODE;
            break;
        case ProcessStatus_TerminatedSignal:
            vrc = EXITCODEEXEC_TERM_SIGNAL;
            break;
        case ProcessStatus_TerminatedAbnormally:
            vrc = EXITCODEEXEC_TERM_ABEND;
            break;
        case ProcessStatus_TimedOutKilled:
            vrc = EXITCODEEXEC_TIMEOUT;
            break;
        case ProcessStatus_TimedOutAbnormally:
            vrc = EXITCODEEXEC_TIMEOUT;
            break;
        case ProcessStatus_Down:
            /* Service/OS is stopping, process was killed, so
             * not exactly an error of the started process ... */
            vrc = EXITCODEEXEC_DOWN;
            break;
        case ProcessStatus_Error:
            vrc = EXITCODEEXEC_FAILED;
            break;
        default:
            AssertMsgFailed(("Unknown exit code (%u) from guest process returned!\n", enmStatus));
            break;
    }
    return vrc;
}

/**
 * Translates a guest session status to a human readable
 * string.
 */
static const char *ctrlSessionStatusToText(GuestSessionStatus_T enmStatus)
{
    switch (enmStatus)
    {
        case GuestSessionStatus_Starting:
            return "starting";
        case GuestSessionStatus_Started:
            return "started";
        case GuestSessionStatus_Terminating:
            return "terminating";
        case GuestSessionStatus_Terminated:
            return "terminated";
        case GuestSessionStatus_TimedOutKilled:
            return "timed out";
        case GuestSessionStatus_TimedOutAbnormally:
            return "timed out, hanging";
        case GuestSessionStatus_Down:
            return "killed";
        case GuestSessionStatus_Error:
            return "error";
        default:
            break;
    }
    return "unknown";
}

static int ctrlPrintError(com::ErrorInfo &errorInfo)
{
    if (   errorInfo.isFullAvailable()
        || errorInfo.isBasicAvailable())
    {
        /* If we got a VBOX_E_IPRT error we handle the error in a more gentle way
         * because it contains more accurate info about what went wrong. */
        if (errorInfo.getResultCode() == VBOX_E_IPRT_ERROR)
            RTMsgError("%ls.", errorInfo.getText().raw());
        else
        {
            RTMsgError("Error details:");
            GluePrintErrorInfo(errorInfo);
        }
        return VERR_GENERAL_FAILURE; /** @todo */
    }
    AssertMsgFailedReturn(("Object has indicated no error (%Rhrc)!?\n", errorInfo.getResultCode()),
                          VERR_INVALID_PARAMETER);
}

static int ctrlPrintError(IUnknown *pObj, const GUID &aIID)
{
    com::ErrorInfo ErrInfo(pObj, aIID);
    return ctrlPrintError(ErrInfo);
}

static int ctrlPrintProgressError(ComPtr<IProgress> pProgress)
{
    int vrc = VINF_SUCCESS;
    HRESULT rc;

    do
    {
        BOOL fCanceled;
        CHECK_ERROR_BREAK(pProgress, COMGETTER(Canceled)(&fCanceled));
        if (!fCanceled)
        {
            LONG rcProc;
            CHECK_ERROR_BREAK(pProgress, COMGETTER(ResultCode)(&rcProc));
            if (FAILED(rcProc))
            {
                com::ProgressErrorInfo ErrInfo(pProgress);
                vrc = ctrlPrintError(ErrInfo);
            }
        }

    } while(0);

    AssertMsgStmt(SUCCEEDED(rc), ("Could not lookup progress information\n"), vrc = VERR_COM_UNEXPECTED);

    return vrc;
}

/**
 * Un-initializes the VM after guest control usage.
 */
static void ctrlUninitVM(HandlerArg *pArg)
{
    AssertPtrReturnVoid(pArg);
    if (pArg->session)
        pArg->session->UnlockMachine();
}

/**
 * Initializes the VM for IGuest operations.
 *
 * That is, checks whether it's up and running, if it can be locked (shared
 * only) and returns a valid IGuest pointer on success.
 *
 * @return  IPRT status code.
 * @param   pArg            Our command line argument structure.
 * @param   pszNameOrId     The VM's name or UUID.
 * @param   pGuest          Where to return the IGuest interface pointer.
 */
static int ctrlInitVM(HandlerArg *pArg, const char *pszNameOrId, ComPtr<IGuest> *pGuest)
{
    AssertPtrReturn(pArg, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszNameOrId, VERR_INVALID_PARAMETER);

    /* Lookup VM. */
    ComPtr<IMachine> machine;
    /* Assume it's an UUID. */
    HRESULT rc;
    CHECK_ERROR(pArg->virtualBox, FindMachine(Bstr(pszNameOrId).raw(),
                                              machine.asOutParam()));
    if (FAILED(rc))
        return VERR_NOT_FOUND;

    /* Machine is running? */
    MachineState_T machineState;
    CHECK_ERROR_RET(machine, COMGETTER(State)(&machineState), 1);
    if (machineState != MachineState_Running)
    {
        RTMsgError("Machine \"%s\" is not running (currently %s)!\n",
                   pszNameOrId, machineStateToName(machineState, false));
        return VERR_VM_INVALID_VM_STATE;
    }

    do
    {
        /* Open a session for the VM. */
        CHECK_ERROR_BREAK(machine, LockMachine(pArg->session, LockType_Shared));
        /* Get the associated console. */
        ComPtr<IConsole> console;
        CHECK_ERROR_BREAK(pArg->session, COMGETTER(Console)(console.asOutParam()));
        /* ... and session machine. */
        ComPtr<IMachine> sessionMachine;
        CHECK_ERROR_BREAK(pArg->session, COMGETTER(Machine)(sessionMachine.asOutParam()));
        /* Get IGuest interface. */
        CHECK_ERROR_BREAK(console, COMGETTER(Guest)(pGuest->asOutParam()));
    } while (0);

    if (FAILED(rc))
        ctrlUninitVM(pArg);
    return SUCCEEDED(rc) ? VINF_SUCCESS : VERR_GENERAL_FAILURE;
}

/**
 * Prints the desired guest output to a stream.
 *
 * @return  IPRT status code.
 * @param   pProcess        Pointer to appropriate process object.
 * @param   pStrmOutput     Where to write the data.
 * @param   uHandle         Handle where to read the data from.
 * @param   uTimeoutMS      Timeout (in ms) to wait for the operation to complete.
 */
static int ctrlExecPrintOutput(IProcess *pProcess, PRTSTREAM pStrmOutput,
                               ULONG uHandle, ULONG uTimeoutMS)
{
    AssertPtrReturn(pProcess, VERR_INVALID_POINTER);
    AssertPtrReturn(pStrmOutput, VERR_INVALID_POINTER);

    int vrc = VINF_SUCCESS;

    SafeArray<BYTE> aOutputData;
    HRESULT rc = pProcess->Read(uHandle, _64K, uTimeoutMS,
                                ComSafeArrayAsOutParam(aOutputData));
    if (FAILED(rc))
        vrc = ctrlPrintError(pProcess, COM_IIDOF(IProcess));
    else
    {
        size_t cbOutputData = aOutputData.size();
        if (cbOutputData > 0)
        {
            BYTE *pBuf = aOutputData.raw();
            AssertPtr(pBuf);
            pBuf[cbOutputData - 1] = 0; /* Properly terminate buffer. */

            /** @todo implement the dos2unix/unix2dos conversions */

            /*
             * If aOutputData is text data from the guest process' stdout or stderr,
             * it has a platform dependent line ending. So standardize on
             * Unix style, as RTStrmWrite does the LF -> CR/LF replacement on
             * Windows. Otherwise we end up with CR/CR/LF on Windows.
             */

            char *pszBufUTF8;
            vrc = RTStrCurrentCPToUtf8(&pszBufUTF8, (const char*)aOutputData.raw());
            if (RT_SUCCESS(vrc))
            {
                cbOutputData = strlen(pszBufUTF8);

                ULONG cbOutputDataPrint = cbOutputData;
                for (char *s = pszBufUTF8, *d = s;
                     s - pszBufUTF8 < (ssize_t)cbOutputData;
                     s++, d++)
                {
                    if (*s == '\r')
                    {
                        /* skip over CR, adjust destination */
                        d--;
                        cbOutputDataPrint--;
                    }
                    else if (s != d)
                        *d = *s;
                }

                vrc = RTStrmWrite(pStrmOutput, pszBufUTF8, cbOutputDataPrint);
                if (RT_FAILURE(vrc))
                    RTMsgError("Unable to write output, rc=%Rrc\n", vrc);

                RTStrFree(pszBufUTF8);
            }
            else
                RTMsgError("Unable to convert output, rc=%Rrc\n", vrc);
        }
    }

    return vrc;
}

/**
 * Returns the remaining time (in ms) based on the start time and a set
 * timeout value. Returns RT_INDEFINITE_WAIT if no timeout was specified.
 *
 * @return  RTMSINTERVAL    Time left (in ms).
 * @param   u64StartMs      Start time (in ms).
 * @param   cMsTimeout      Timeout value (in ms).
 */
inline RTMSINTERVAL ctrlExecGetRemainingTime(uint64_t u64StartMs, RTMSINTERVAL cMsTimeout)
{
    if (!cMsTimeout || cMsTimeout == RT_INDEFINITE_WAIT) /* If no timeout specified, wait forever. */
        return RT_INDEFINITE_WAIT;

    uint32_t u64ElapsedMs = RTTimeMilliTS() - u64StartMs;
    if (u64ElapsedMs >= cMsTimeout)
        return 0;

    return cMsTimeout - u64ElapsedMs;
}

/* <Missing documentation> */
static RTEXITCODE handleCtrlProcessExec(ComPtr<IGuest> pGuest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    /*
     * Parse arguments.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--dos2unix",                     GETOPTDEF_EXEC_DOS2UNIX,                  RTGETOPT_REQ_NOTHING },
        { "--environment",                  'e',                                      RTGETOPT_REQ_STRING  },
        { "--flags",                        'f',                                      RTGETOPT_REQ_STRING  },
        { "--ignore-operhaned-processes",   GETOPTDEF_EXEC_IGNOREORPHANEDPROCESSES,   RTGETOPT_REQ_NOTHING },
        { "--image",                        'i',                                      RTGETOPT_REQ_STRING  },
        { "--no-profile",                   GETOPTDEF_EXEC_NO_PROFILE,                RTGETOPT_REQ_NOTHING },
        { "--username",                     'u',                                      RTGETOPT_REQ_STRING  },
        { "--passwordfile",                 'p',                                      RTGETOPT_REQ_STRING  },
        { "--password",                     GETOPTDEF_EXEC_PASSWORD,                  RTGETOPT_REQ_STRING  },
        { "--domain",                       'd',                                      RTGETOPT_REQ_STRING  },
        { "--timeout",                      't',                                      RTGETOPT_REQ_UINT32  },
        { "--unix2dos",                     GETOPTDEF_EXEC_UNIX2DOS,                  RTGETOPT_REQ_NOTHING },
        { "--verbose",                      'v',                                      RTGETOPT_REQ_NOTHING },
        { "--wait-exit",                    GETOPTDEF_EXEC_WAITFOREXIT,               RTGETOPT_REQ_NOTHING },
        { "--wait-stdout",                  GETOPTDEF_EXEC_WAITFORSTDOUT,             RTGETOPT_REQ_NOTHING },
        { "--wait-stderr",                  GETOPTDEF_EXEC_WAITFORSTDERR,             RTGETOPT_REQ_NOTHING }
    };

    int                     ch;
    RTGETOPTUNION           ValueUnion;
    RTGETOPTSTATE           GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv, s_aOptions, RT_ELEMENTS(s_aOptions), 0, 0);

    Utf8Str                              strCmd;
    com::SafeArray<ProcessCreateFlag_T>  aCreateFlags;
    com::SafeArray<ProcessWaitForFlag_T> aWaitFlags;
    com::SafeArray<IN_BSTR>              aArgs;
    com::SafeArray<IN_BSTR>              aEnv;
    Utf8Str                              strUsername;
    Utf8Str                              strPassword;
    Utf8Str                              strDomain;
    RTMSINTERVAL                         cMsTimeout      = 0;
    OUTPUTTYPE                           eOutputType     = OUTPUTTYPE_UNDEFINED;
    bool                                 fDetached       = true;
    bool                                 fVerbose        = false;
    int                                  vrc             = VINF_SUCCESS;

    try
    {
        /* Wait for process start in any case. This is useful for scripting VBoxManage
         * when relying on its overall exit code. */
        aWaitFlags.push_back(ProcessWaitForFlag_Start);

        while (   (ch = RTGetOpt(&GetState, &ValueUnion))
               && RT_SUCCESS(vrc))
        {
            /* For options that require an argument, ValueUnion has received the value. */
            switch (ch)
            {
                case GETOPTDEF_EXEC_DOS2UNIX:
                    if (eOutputType != OUTPUTTYPE_UNDEFINED)
                        return errorSyntax(USAGE_GUESTCONTROL, "More than one output type (dos2unix/unix2dos) specified!");
                    eOutputType = OUTPUTTYPE_DOS2UNIX;
                    break;

                case 'e': /* Environment */
                {
                    char **papszArg;
                    int cArgs;

                    vrc = RTGetOptArgvFromString(&papszArg, &cArgs, ValueUnion.psz, NULL);
                    if (RT_FAILURE(vrc))
                        return errorSyntax(USAGE_GUESTCONTROL, "Failed to parse environment value, rc=%Rrc", vrc);
                    for (int j = 0; j < cArgs; j++)
                        aEnv.push_back(Bstr(papszArg[j]).raw());

                    RTGetOptArgvFree(papszArg);
                    break;
                }

                case GETOPTDEF_EXEC_IGNOREORPHANEDPROCESSES:
                    aCreateFlags.push_back(ProcessCreateFlag_IgnoreOrphanedProcesses);
                    break;

                case GETOPTDEF_EXEC_NO_PROFILE:
                    aCreateFlags.push_back(ProcessCreateFlag_NoProfile);
                    break;

                case 'i':
                    strCmd = ValueUnion.psz;
                    break;

                /** @todo Add a hidden flag. */

                case 'u': /* User name */
                    strUsername = ValueUnion.psz;
                    break;

                case GETOPTDEF_EXEC_PASSWORD: /* Password */
                    strPassword = ValueUnion.psz;
                    break;

                case 'p': /* Password file */
                {
                    RTEXITCODE rcExit = readPasswordFile(ValueUnion.psz, &strPassword);
                    if (rcExit != RTEXITCODE_SUCCESS)
                        return rcExit;
                    break;
                }

                case 'd': /* domain */
                    strDomain = ValueUnion.psz;
                    break;

                case 't': /* Timeout */
                    cMsTimeout = ValueUnion.u32;
                    break;

                case GETOPTDEF_EXEC_UNIX2DOS:
                    if (eOutputType != OUTPUTTYPE_UNDEFINED)
                        return errorSyntax(USAGE_GUESTCONTROL, "More than one output type (dos2unix/unix2dos) specified!");
                    eOutputType = OUTPUTTYPE_UNIX2DOS;
                    break;

                case 'v': /* Verbose */
                    fVerbose = true;
                    break;

                case GETOPTDEF_EXEC_WAITFOREXIT:
                    aWaitFlags.push_back(ProcessWaitForFlag_Terminate);
                    fDetached = false;
                    break;

                case GETOPTDEF_EXEC_WAITFORSTDOUT:
                    aCreateFlags.push_back(ProcessCreateFlag_WaitForStdOut);
                    aWaitFlags.push_back(ProcessWaitForFlag_StdOut);
                    fDetached = false;
                    break;

                case GETOPTDEF_EXEC_WAITFORSTDERR:
                    aCreateFlags.push_back(ProcessCreateFlag_WaitForStdErr);
                    aWaitFlags.push_back(ProcessWaitForFlag_StdErr);
                    fDetached = false;
                    break;

                case VINF_GETOPT_NOT_OPTION:
                {
                    if (aArgs.size() == 0 && strCmd.isEmpty())
                        strCmd = ValueUnion.psz;
                    else
                        aArgs.push_back(Bstr(ValueUnion.psz).raw());
                    break;
                }

                default:
                    return RTGetOptPrintError(ch, &ValueUnion);

            } /* switch */
        } /* while RTGetOpt */
    }
    catch (std::bad_alloc &)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return RTMsgErrorExit(RTEXITCODE_FAILURE, "Failed to initialize, rc=%Rrc\n", vrc);

    if (strCmd.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No command to execute specified!");

    if (strUsername.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No user name specified!");

    /** @todo Any output conversion not supported yet! */
    if (eOutputType != OUTPUTTYPE_UNDEFINED)
        return errorSyntax(USAGE_GUESTCONTROL, "Output conversion not implemented yet!");

    ctrlSignalHandlerInstall();

    /*
     * Start with the real work.
     */
    HRESULT rc = S_OK;
    if (fVerbose)
        RTPrintf("Opening guest session as user '%s' ...\n", strUsername.c_str());

    RTEXITCODE rcExit = RTEXITCODE_SUCCESS;

    /** @todo This eventually needs a bit of revamping so that a valid session gets passed
     *        into this function already so that we don't need to mess around with closing
     *        the session all over the places below again. Later. */

    try
    {
        do
        {
            Utf8Str strVBoxManage;
            strVBoxManage.printf("VBoxManage Guest Control (PID %RU32)", RTProcSelf());

            CHECK_ERROR_BREAK(pGuest, CreateSession(Bstr(strUsername).raw(),
                                      Bstr(strPassword).raw(),
                                      Bstr(strDomain).raw(),
                                      Bstr(strVBoxManage).raw(),
                                      g_pGuestSession.asOutParam()));

            /* Adjust process creation flags if we don't want to wait for process termination. */
            if (fDetached)
                aCreateFlags.push_back(ProcessCreateFlag_WaitForProcessStartOnly);

            /* Get current time stamp to later calculate rest of timeout left. */
            uint64_t u64StartMS = RTTimeMilliTS();

            /*
             * Wait for guest session to start.
             */
            if (fVerbose)
            {
                if (cMsTimeout == 0)
                    RTPrintf("Waiting for guest session to start ...\n");
                else
                    RTPrintf("Waiting for guest session to start (within %ums)\n", cMsTimeout);
            }

            com::SafeArray<GuestSessionWaitForFlag_T> aSessionWaitFlags;
            aSessionWaitFlags.push_back(GuestSessionWaitForFlag_Start);
            GuestSessionWaitResult_T sessionWaitResult;
            CHECK_ERROR_BREAK(g_pGuestSession, WaitForArray(ComSafeArrayAsInParam(aSessionWaitFlags), cMsTimeout, &sessionWaitResult));
            ULONG uSessionID;
            CHECK_ERROR_BREAK(g_pGuestSession, COMGETTER(Id)(&uSessionID));

            if (   sessionWaitResult == GuestSessionWaitResult_Start
                /* Note: This might happen when Guest Additions < 4.3 are installed which don't
                 *       support dedicated guest sessions. */
                || sessionWaitResult == GuestSessionWaitResult_WaitFlagNotSupported)
            {
                if (fVerbose)
                    RTPrintf("Guest session (ID %RU32) has been started\n", uSessionID);
            }
            else
            {
                RTPrintf("Error starting guest session\n");
                break;
            }

            if (fVerbose)
            {
                if (cMsTimeout == 0)
                    RTPrintf("Waiting for guest process to start ...\n");
                else
                    RTPrintf("Waiting for guest process to start (within %ums)\n", cMsTimeout);
            }

            /*
             * Execute the process.
             */
            ComPtr<IGuestProcess> pProcess;
            CHECK_ERROR_BREAK(g_pGuestSession, ProcessCreate(Bstr(strCmd).raw(),
                                                           ComSafeArrayAsInParam(aArgs),
                                                           ComSafeArrayAsInParam(aEnv),
                                                           ComSafeArrayAsInParam(aCreateFlags),
                                                           cMsTimeout,
                                                           pProcess.asOutParam()));

            /** @todo does this need signal handling? there's no progress object etc etc */

            vrc = RTStrmSetMode(g_pStdOut, 1 /* Binary mode */, -1 /* Code set, unchanged */);
            if (RT_FAILURE(vrc))
                RTMsgError("Unable to set stdout's binary mode, rc=%Rrc\n", vrc);
            vrc = RTStrmSetMode(g_pStdErr, 1 /* Binary mode */, -1 /* Code set, unchanged */);
            if (RT_FAILURE(vrc))
                RTMsgError("Unable to set stderr's binary mode, rc=%Rrc\n", vrc);

            /* Wait for process to exit ... */
            RTMSINTERVAL cMsTimeLeft = 1;
            bool fReadStdOut, fReadStdErr;
            fReadStdOut = fReadStdErr = false;

            bool fCompleted = false;
            while (!fCompleted && cMsTimeLeft != 0)
            {
                cMsTimeLeft = ctrlExecGetRemainingTime(u64StartMS, cMsTimeout);
                ProcessWaitResult_T waitResult;
                CHECK_ERROR_BREAK(pProcess, WaitForArray(ComSafeArrayAsInParam(aWaitFlags),
                                                         cMsTimeLeft, &waitResult));
                switch (waitResult)
                {
                    case ProcessWaitResult_Start:
                    {
                        ULONG uPID = 0;
                        CHECK_ERROR_BREAK(pProcess, COMGETTER(PID)(&uPID));
                        if (fVerbose)
                        {
                            RTPrintf("Process '%s' (PID %RU32) started\n",
                                     strCmd.c_str(), uPID);
                        }
                        else /** @todo Introduce a --quiet option for not printing this. */
                        {
                            /* Just print plain PID to make it easier for scripts
                             * invoking VBoxManage. */
                            RTPrintf("%RU32, session ID %RU32\n", uPID, uSessionID);
                        }

                        /* We're done here if we don't want to wait for termination. */
                        if (fDetached)
                            fCompleted = true;

                        break;
                    }
                    case ProcessWaitResult_StdOut:
                        fReadStdOut = true;
                        break;
                    case ProcessWaitResult_StdErr:
                        fReadStdErr = true;
                        break;
                    case ProcessWaitResult_Terminate:
                        /* Process terminated, we're done */
                        fCompleted = true;
                        break;
                    case ProcessWaitResult_WaitFlagNotSupported:
                    {
                        /* The guest does not support waiting for stdout/err, so
                         * yield to reduce the CPU load due to busy waiting. */
                        RTThreadYield(); /* Optional, don't check rc. */

                        /* Try both, stdout + stderr. */
                        fReadStdOut = fReadStdErr = true;
                        break;
                    }
                    default:
                        /* Ignore all other results, let the timeout expire */
                        break;
                }

                if (g_fGuestCtrlCanceled)
                    break;

                if (fReadStdOut) /* Do we need to fetch stdout data? */
                {
                    cMsTimeLeft = ctrlExecGetRemainingTime(u64StartMS, cMsTimeout);
                    vrc = ctrlExecPrintOutput(pProcess, g_pStdOut,
                                              1 /* StdOut */, cMsTimeLeft);
                    fReadStdOut = false;
                }

                if (fReadStdErr) /* Do we need to fetch stdout data? */
                {
                    cMsTimeLeft = ctrlExecGetRemainingTime(u64StartMS, cMsTimeout);
                    vrc = ctrlExecPrintOutput(pProcess, g_pStdErr,
                                              2 /* StdErr */, cMsTimeLeft);
                    fReadStdErr = false;
                }

                if (   RT_FAILURE(vrc)
                    || g_fGuestCtrlCanceled)
                    break;

                /* Did we run out of time? */
                if (   cMsTimeout
                    && RTTimeMilliTS() - u64StartMS > cMsTimeout)
                    break;

                NativeEventQueue::getMainEventQueue()->processEventQueue(0);

            } /* while */

            /* Report status back to the user. */
            if (   fCompleted
                && !g_fGuestCtrlCanceled)
            {
                ProcessStatus_T procStatus;
                CHECK_ERROR_BREAK(pProcess, COMGETTER(Status)(&procStatus));
                if (   procStatus == ProcessStatus_TerminatedNormally
                    || procStatus == ProcessStatus_TerminatedAbnormally
                    || procStatus == ProcessStatus_TerminatedSignal)
                {
                    LONG exitCode;
                    CHECK_ERROR_BREAK(pProcess, COMGETTER(ExitCode)(&exitCode));
                    if (fVerbose)
                        RTPrintf("Exit code=%u (Status=%u [%s])\n",
                                 exitCode, procStatus, ctrlExecProcessStatusToText(procStatus));

                    rcExit = (RTEXITCODE)ctrlExecProcessStatusToExitCode(procStatus, exitCode);
                }
                else if (fVerbose)
                    RTPrintf("Process now is in status [%s]\n", ctrlExecProcessStatusToText(procStatus));
            }
            else
            {
                if (fVerbose)
                    RTPrintf("Process execution aborted!\n");

                rcExit = (RTEXITCODE)EXITCODEEXEC_TERM_ABEND;
            }

        } while (0);
    }
    catch (std::bad_alloc)
    {
        rc = E_OUTOFMEMORY;
    }

    ctrlSignalHandlerUninstall();

    bool fCloseSession = false;
    if (SUCCEEDED(rc))
    {
        /*
         * Only close the guest session if we waited for the guest
         * process to exit. Otherwise we wouldn't have any chance to
         * access and/or kill detached guest process lateron.
         */
        fCloseSession = !fDetached;
    }
    else /* Close session on error. */
        fCloseSession = true;

    if (   fCloseSession
        && !g_pGuestSession.isNull())
    {
        if (fVerbose)
            RTPrintf("Closing guest session ...\n");
        rc = g_pGuestSession->Close();
    }
    else if (!fCloseSession && fVerbose)
        RTPrintf("Guest session detached\n");

    if (   rcExit == RTEXITCODE_SUCCESS
        && FAILED(rc))
    {
        /* Make sure an appropriate exit code is set on error. */
        rcExit = RTEXITCODE_FAILURE;
    }

    return rcExit;
}

/**
 * Creates a copy context structure which then can be used with various
 * guest control copy functions. Needs to be free'd with ctrlCopyContextFree().
 *
 * @return  IPRT status code.
 * @param   pGuest                  Pointer to IGuest interface to use.
 * @param   fVerbose                Flag indicating if we want to run in verbose mode.
 * @param   fDryRun                 Flag indicating if we want to run a dry run only.
 * @param   fHostToGuest            Flag indicating if we want to copy from host to guest
 *                                  or vice versa.
 * @param   strUsername             Username of account to use on the guest side.
 * @param   strPassword             Password of account to use.
 * @param   strDomain               Domain of account to use.
 * @param   strSessionName          Session name (only for identification purposes).
 * @param   ppContext               Pointer which receives the allocated copy context.
 */
static int ctrlCopyContextCreate(IGuest *pGuest, bool fVerbose, bool fDryRun,
                                 bool fHostToGuest, const Utf8Str &strUsername,
                                 const Utf8Str &strPassword, const Utf8Str &strDomain,
                                 const Utf8Str &strSessionName,
                                 PCOPYCONTEXT *ppContext)
{
    AssertPtrReturn(pGuest, VERR_INVALID_POINTER);

    PCOPYCONTEXT pContext = new COPYCONTEXT();
    AssertPtrReturn(pContext, VERR_NO_MEMORY); /**< @todo r=klaus cannot happen with new */
    ComPtr<IGuestSession> pGuestSession;
    HRESULT rc = pGuest->CreateSession(Bstr(strUsername).raw(),
                                       Bstr(strPassword).raw(),
                                       Bstr(strDomain).raw(),
                                       Bstr(strSessionName).raw(),
                                       pGuestSession.asOutParam());
    if (FAILED(rc))
        return ctrlPrintError(pGuest, COM_IIDOF(IGuest));

    pContext->fVerbose = fVerbose;
    pContext->fDryRun = fDryRun;
    pContext->fHostToGuest = fHostToGuest;
    pContext->pGuestSession = pGuestSession;

    *ppContext = pContext;

    return VINF_SUCCESS;
}

/**
 * Frees are previously allocated copy context structure.
 *
 * @param   pContext                Pointer to copy context to free.
 */
static void ctrlCopyContextFree(PCOPYCONTEXT pContext)
{
    if (pContext)
    {
        if (pContext->pGuestSession)
            pContext->pGuestSession->Close();
        delete pContext;
    }
}

/**
 * Translates a source path to a destination path (can be both sides,
 * either host or guest). The source root is needed to determine the start
 * of the relative source path which also needs to present in the destination
 * path.
 *
 * @return  IPRT status code.
 * @param   pszSourceRoot           Source root path. No trailing directory slash!
 * @param   pszSource               Actual source to transform. Must begin with
 *                                  the source root path!
 * @param   pszDest                 Destination path.
 * @param   ppszTranslated          Pointer to the allocated, translated destination
 *                                  path. Must be free'd with RTStrFree().
 */
static int ctrlCopyTranslatePath(const char *pszSourceRoot, const char *pszSource,
                                 const char *pszDest, char **ppszTranslated)
{
    AssertPtrReturn(pszSourceRoot, VERR_INVALID_POINTER);
    AssertPtrReturn(pszSource, VERR_INVALID_POINTER);
    AssertPtrReturn(pszDest, VERR_INVALID_POINTER);
    AssertPtrReturn(ppszTranslated, VERR_INVALID_POINTER);
#if 0 /** @todo r=bird: It does not make sense to apply host path parsing semantics onto guest paths. I hope this code isn't mixing host/guest paths in the same way anywhere else... @bugref{6344} */
    AssertReturn(RTPathStartsWith(pszSource, pszSourceRoot), VERR_INVALID_PARAMETER);
#endif

    /* Construct the relative dest destination path by "subtracting" the
     * source from the source root, e.g.
     *
     * source root path = "e:\foo\", source = "e:\foo\bar"
     * dest = "d:\baz\"
     * translated = "d:\baz\bar\"
     */
    char szTranslated[RTPATH_MAX];
    size_t srcOff = strlen(pszSourceRoot);
    AssertReturn(srcOff, VERR_INVALID_PARAMETER);

    char *pszDestPath = RTStrDup(pszDest);
    AssertPtrReturn(pszDestPath, VERR_NO_MEMORY);

    int vrc;
    if (!RTPathFilename(pszDestPath))
    {
        vrc = RTPathJoin(szTranslated, sizeof(szTranslated),
                         pszDestPath, &pszSource[srcOff]);
    }
    else
    {
        char *pszDestFileName = RTStrDup(RTPathFilename(pszDestPath));
        if (pszDestFileName)
        {
            RTPathStripFilename(pszDestPath);
            vrc = RTPathJoin(szTranslated, sizeof(szTranslated),
                            pszDestPath, pszDestFileName);
            RTStrFree(pszDestFileName);
        }
        else
            vrc = VERR_NO_MEMORY;
    }
    RTStrFree(pszDestPath);

    if (RT_SUCCESS(vrc))
    {
        *ppszTranslated = RTStrDup(szTranslated);
#if 0
        RTPrintf("Root: %s, Source: %s, Dest: %s, Translated: %s\n",
                 pszSourceRoot, pszSource, pszDest, *ppszTranslated);
#endif
    }
    return vrc;
}

#ifdef DEBUG_andy
static int tstTranslatePath()
{
    RTAssertSetMayPanic(false /* Do not freak out, please. */);

    static struct
    {
        const char *pszSourceRoot;
        const char *pszSource;
        const char *pszDest;
        const char *pszTranslated;
        int         iResult;
    } aTests[] =
    {
        /* Invalid stuff. */
        { NULL, NULL, NULL, NULL, VERR_INVALID_POINTER },
#ifdef RT_OS_WINDOWS
        /* Windows paths. */
        { "c:\\foo", "c:\\foo\\bar.txt", "c:\\test", "c:\\test\\bar.txt", VINF_SUCCESS },
        { "c:\\foo", "c:\\foo\\baz\\bar.txt", "c:\\test", "c:\\test\\baz\\bar.txt", VINF_SUCCESS },
#else /* RT_OS_WINDOWS */
        { "/home/test/foo", "/home/test/foo/bar.txt", "/opt/test", "/opt/test/bar.txt", VINF_SUCCESS },
        { "/home/test/foo", "/home/test/foo/baz/bar.txt", "/opt/test", "/opt/test/baz/bar.txt", VINF_SUCCESS },
#endif /* !RT_OS_WINDOWS */
        /* Mixed paths*/
        /** @todo */
        { NULL }
    };

    size_t iTest = 0;
    for (iTest; iTest < RT_ELEMENTS(aTests); iTest++)
    {
        RTPrintf("=> Test %d\n", iTest);
        RTPrintf("\tSourceRoot=%s, Source=%s, Dest=%s\n",
                 aTests[iTest].pszSourceRoot, aTests[iTest].pszSource, aTests[iTest].pszDest);

        char *pszTranslated = NULL;
        int iResult =  ctrlCopyTranslatePath(aTests[iTest].pszSourceRoot, aTests[iTest].pszSource,
                                             aTests[iTest].pszDest, &pszTranslated);
        if (iResult != aTests[iTest].iResult)
        {
            RTPrintf("\tReturned %Rrc, expected %Rrc\n",
                     iResult, aTests[iTest].iResult);
        }
        else if (   pszTranslated
                 && strcmp(pszTranslated, aTests[iTest].pszTranslated))
        {
            RTPrintf("\tReturned translated path %s, expected %s\n",
                     pszTranslated, aTests[iTest].pszTranslated);
        }

        if (pszTranslated)
        {
            RTPrintf("\tTranslated=%s\n", pszTranslated);
            RTStrFree(pszTranslated);
        }
    }

    return VINF_SUCCESS; /* @todo */
}
#endif

/**
 * Creates a directory on the destination, based on the current copy
 * context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszDir                  Directory to create.
 */
static int ctrlCopyDirCreate(PCOPYCONTEXT pContext, const char *pszDir)
{
    AssertPtrReturn(pContext, VERR_INVALID_POINTER);
    AssertPtrReturn(pszDir, VERR_INVALID_POINTER);

    bool fDirExists;
    int vrc = ctrlCopyDirExists(pContext, pContext->fHostToGuest, pszDir, &fDirExists);
    if (   RT_SUCCESS(vrc)
        && fDirExists)
    {
        if (pContext->fVerbose)
            RTPrintf("Directory \"%s\" already exists\n", pszDir);
        return VINF_SUCCESS;
    }

    /* If querying for a directory existence fails there's no point of even trying
     * to create such a directory. */
    if (RT_FAILURE(vrc))
        return vrc;

    if (pContext->fVerbose)
        RTPrintf("Creating directory \"%s\" ...\n", pszDir);

    if (pContext->fDryRun)
        return VINF_SUCCESS;

    if (pContext->fHostToGuest) /* We want to create directories on the guest. */
    {
        SafeArray<DirectoryCreateFlag_T> dirCreateFlags;
        dirCreateFlags.push_back(DirectoryCreateFlag_Parents);
        HRESULT rc = pContext->pGuestSession->DirectoryCreate(Bstr(pszDir).raw(),
                                                              0700, ComSafeArrayAsInParam(dirCreateFlags));
        if (FAILED(rc))
            vrc = ctrlPrintError(pContext->pGuestSession, COM_IIDOF(IGuestSession));
    }
    else /* ... or on the host. */
    {
        vrc = RTDirCreateFullPath(pszDir, 0700);
        if (vrc == VERR_ALREADY_EXISTS)
            vrc = VINF_SUCCESS;
    }
    return vrc;
}

/**
 * Checks whether a specific host/guest directory exists.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   bGuest                  true if directory needs to be checked on the guest
 *                                  or false if on the host.
 * @param   pszDir                  Actual directory to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given directory exists or not.
 */
static int ctrlCopyDirExists(PCOPYCONTEXT pContext, bool bGuest,
                             const char *pszDir, bool *fExists)
{
    AssertPtrReturn(pContext, false);
    AssertPtrReturn(pszDir, false);
    AssertPtrReturn(fExists, false);

    int vrc = VINF_SUCCESS;
    if (bGuest)
    {
        BOOL fDirExists = FALSE;
        HRESULT rc = pContext->pGuestSession->DirectoryExists(Bstr(pszDir).raw(), &fDirExists);
        if (FAILED(rc))
            vrc = ctrlPrintError(pContext->pGuestSession, COM_IIDOF(IGuestSession));
        else
            *fExists = fDirExists ? true : false;
    }
    else
        *fExists = RTDirExists(pszDir);
    return vrc;
}

/**
 * Checks whether a specific directory exists on the destination, based
 * on the current copy context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszDir                  Actual directory to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given directory exists or not.
 */
static int ctrlCopyDirExistsOnDest(PCOPYCONTEXT pContext, const char *pszDir,
                                   bool *fExists)
{
    return ctrlCopyDirExists(pContext, pContext->fHostToGuest,
                             pszDir, fExists);
}

/**
 * Checks whether a specific directory exists on the source, based
 * on the current copy context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszDir                  Actual directory to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given directory exists or not.
 */
static int ctrlCopyDirExistsOnSource(PCOPYCONTEXT pContext, const char *pszDir,
                                     bool *fExists)
{
    return ctrlCopyDirExists(pContext, !pContext->fHostToGuest,
                             pszDir, fExists);
}

/**
 * Checks whether a specific host/guest file exists.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   bGuest                  true if file needs to be checked on the guest
 *                                  or false if on the host.
 * @param   pszFile                 Actual file to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given file exists or not.
 */
static int ctrlCopyFileExists(PCOPYCONTEXT pContext, bool bOnGuest,
                              const char *pszFile, bool *fExists)
{
    AssertPtrReturn(pContext, false);
    AssertPtrReturn(pszFile, false);
    AssertPtrReturn(fExists, false);

    int vrc = VINF_SUCCESS;
    if (bOnGuest)
    {
        BOOL fFileExists = FALSE;
        HRESULT rc = pContext->pGuestSession->FileExists(Bstr(pszFile).raw(), &fFileExists);
        if (FAILED(rc))
            vrc = ctrlPrintError(pContext->pGuestSession, COM_IIDOF(IGuestSession));
        else
            *fExists = fFileExists ? true : false;
    }
    else
        *fExists = RTFileExists(pszFile);
    return vrc;
}

/**
 * Checks whether a specific file exists on the destination, based on the
 * current copy context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszFile                 Actual file to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given file exists or not.
 */
static int ctrlCopyFileExistsOnDest(PCOPYCONTEXT pContext, const char *pszFile,
                                    bool *fExists)
{
    return ctrlCopyFileExists(pContext, pContext->fHostToGuest,
                              pszFile, fExists);
}

/**
 * Checks whether a specific file exists on the source, based on the
 * current copy context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszFile                 Actual file to check.
 * @param   fExists                 Pointer which receives the result if the
 *                                  given file exists or not.
 */
static int ctrlCopyFileExistsOnSource(PCOPYCONTEXT pContext, const char *pszFile,
                                      bool *fExists)
{
    return ctrlCopyFileExists(pContext, !pContext->fHostToGuest,
                              pszFile, fExists);
}

/**
 * Copies a source file to the destination.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszFileSource           Source file to copy to the destination.
 * @param   pszFileDest             Name of copied file on the destination.
 * @param   fFlags                  Copy flags. No supported at the moment and needs
 *                                  to be set to 0.
 */
static int ctrlCopyFileToDest(PCOPYCONTEXT pContext, const char *pszFileSource,
                              const char *pszFileDest, uint32_t fFlags)
{
    AssertPtrReturn(pContext, VERR_INVALID_POINTER);
    AssertPtrReturn(pszFileSource, VERR_INVALID_POINTER);
    AssertPtrReturn(pszFileDest, VERR_INVALID_POINTER);
    AssertReturn(!fFlags, VERR_INVALID_POINTER); /* No flags supported yet. */

    if (pContext->fVerbose)
        RTPrintf("Copying \"%s\" to \"%s\" ...\n",
                 pszFileSource, pszFileDest);

    if (pContext->fDryRun)
        return VINF_SUCCESS;

    int vrc = VINF_SUCCESS;
    ComPtr<IProgress> pProgress;
    HRESULT rc;
    if (pContext->fHostToGuest)
    {
        SafeArray<CopyFileFlag_T> copyFlags;
        rc = pContext->pGuestSession->CopyTo(Bstr(pszFileSource).raw(), Bstr(pszFileDest).raw(),
                                             ComSafeArrayAsInParam(copyFlags),

                                             pProgress.asOutParam());
    }
    else
    {
        SafeArray<CopyFileFlag_T> copyFlags;
        rc = pContext->pGuestSession->CopyFrom(Bstr(pszFileSource).raw(), Bstr(pszFileDest).raw(),
                                               ComSafeArrayAsInParam(copyFlags),
                                               pProgress.asOutParam());
    }

    if (FAILED(rc))
    {
        vrc = ctrlPrintError(pContext->pGuestSession, COM_IIDOF(IGuestSession));
    }
    else
    {
        if (pContext->fVerbose)
            rc = showProgress(pProgress);
        else
            rc = pProgress->WaitForCompletion(-1 /* No timeout */);
        if (SUCCEEDED(rc))
            CHECK_PROGRESS_ERROR(pProgress, ("File copy failed"));
        vrc = ctrlPrintProgressError(pProgress);
    }

    return vrc;
}

/**
 * Copys a directory (tree) from host to the guest.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszSource               Source directory on the host to copy to the guest.
 * @param   pszFilter               DOS-style wildcard filter (?, *).  Optional.
 * @param   pszDest                 Destination directory on the guest.
 * @param   fFlags                  Copy flags, such as recursive copying.
 * @param   pszSubDir               Current sub directory to handle. Needs to NULL and only
 *                                  is needed for recursion.
 */
static int ctrlCopyDirToGuest(PCOPYCONTEXT pContext,
                              const char *pszSource, const char *pszFilter,
                              const char *pszDest, uint32_t fFlags,
                              const char *pszSubDir /* For recursion. */)
{
    AssertPtrReturn(pContext, VERR_INVALID_POINTER);
    AssertPtrReturn(pszSource, VERR_INVALID_POINTER);
    /* Filter is optional. */
    AssertPtrReturn(pszDest, VERR_INVALID_POINTER);
    /* Sub directory is optional. */

    /*
     * Construct current path.
     */
    char szCurDir[RTPATH_MAX];
    int vrc = RTStrCopy(szCurDir, sizeof(szCurDir), pszSource);
    if (RT_SUCCESS(vrc) && pszSubDir)
        vrc = RTPathAppend(szCurDir, sizeof(szCurDir), pszSubDir);

    if (pContext->fVerbose)
        RTPrintf("Processing host directory: %s\n", szCurDir);

    /* Flag indicating whether the current directory was created on the
     * target or not. */
    bool fDirCreated = false;

    /*
     * Open directory without a filter - RTDirOpenFiltered unfortunately
     * cannot handle sub directories so we have to do the filtering ourselves.
     */
    PRTDIR pDir = NULL;
    if (RT_SUCCESS(vrc))
    {
        vrc = RTDirOpen(&pDir, szCurDir);
        if (RT_FAILURE(vrc))
            pDir = NULL;
    }
    if (RT_SUCCESS(vrc))
    {
        /*
         * Enumerate the directory tree.
         */
        while (RT_SUCCESS(vrc))
        {
            RTDIRENTRY DirEntry;
            vrc = RTDirRead(pDir, &DirEntry, NULL);
            if (RT_FAILURE(vrc))
            {
                if (vrc == VERR_NO_MORE_FILES)
                    vrc = VINF_SUCCESS;
                break;
            }
            /** @todo r=bird: This ain't gonna work on most UNIX file systems because
             *        enmType is RTDIRENTRYTYPE_UNKNOWN.  This is clearly documented in
             *        RTDIRENTRY::enmType. For trunk, RTDirQueryUnknownType can be used. */
            switch (DirEntry.enmType)
            {
                case RTDIRENTRYTYPE_DIRECTORY:
                {
                    /* Skip "." and ".." entries. */
                    if (   !strcmp(DirEntry.szName, ".")
                        || !strcmp(DirEntry.szName, ".."))
                        break;

                    if (pContext->fVerbose)
                        RTPrintf("Directory: %s\n", DirEntry.szName);

                    if (fFlags & CopyFileFlag_Recursive)
                    {
                        char *pszNewSub = NULL;
                        if (pszSubDir)
                            pszNewSub = RTPathJoinA(pszSubDir, DirEntry.szName);
                        else
                        {
                            pszNewSub = RTStrDup(DirEntry.szName);
                            RTPathStripTrailingSlash(pszNewSub);
                        }

                        if (pszNewSub)
                        {
                            vrc = ctrlCopyDirToGuest(pContext,
                                                     pszSource, pszFilter,
                                                     pszDest, fFlags, pszNewSub);
                            RTStrFree(pszNewSub);
                        }
                        else
                            vrc = VERR_NO_MEMORY;
                    }
                    break;
                }

                case RTDIRENTRYTYPE_SYMLINK:
                    if (   (fFlags & CopyFileFlag_Recursive)
                        && (fFlags & CopyFileFlag_FollowLinks))
                    {
                        /* Fall through to next case is intentional. */
                    }
                    else
                        break;

                case RTDIRENTRYTYPE_FILE:
                {
                    if (   pszFilter
                        && !RTStrSimplePatternMatch(pszFilter, DirEntry.szName))
                    {
                        break; /* Filter does not match. */
                    }

                    if (pContext->fVerbose)
                        RTPrintf("File: %s\n", DirEntry.szName);

                    if (!fDirCreated)
                    {
                        char *pszDestDir;
                        vrc = ctrlCopyTranslatePath(pszSource, szCurDir,
                                                    pszDest, &pszDestDir);
                        if (RT_SUCCESS(vrc))
                        {
                            vrc = ctrlCopyDirCreate(pContext, pszDestDir);
                            RTStrFree(pszDestDir);

                            fDirCreated = true;
                        }
                    }

                    if (RT_SUCCESS(vrc))
                    {
                        char *pszFileSource = RTPathJoinA(szCurDir, DirEntry.szName);
                        if (pszFileSource)
                        {
                            char *pszFileDest;
                            vrc = ctrlCopyTranslatePath(pszSource, pszFileSource,
                                                       pszDest, &pszFileDest);
                            if (RT_SUCCESS(vrc))
                            {
                                vrc = ctrlCopyFileToDest(pContext, pszFileSource,
                                                        pszFileDest, 0 /* Flags */);
                                RTStrFree(pszFileDest);
                            }
                            RTStrFree(pszFileSource);
                        }
                    }
                    break;
                }

                default:
                    break;
            }
            if (RT_FAILURE(vrc))
                break;
        }

        RTDirClose(pDir);
    }
    return vrc;
}

/**
 * Copys a directory (tree) from guest to the host.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszSource               Source directory on the guest to copy to the host.
 * @param   pszFilter               DOS-style wildcard filter (?, *).  Optional.
 * @param   pszDest                 Destination directory on the host.
 * @param   fFlags                  Copy flags, such as recursive copying.
 * @param   pszSubDir               Current sub directory to handle. Needs to NULL and only
 *                                  is needed for recursion.
 */
static int ctrlCopyDirToHost(PCOPYCONTEXT pContext,
                             const char *pszSource, const char *pszFilter,
                             const char *pszDest, uint32_t fFlags,
                             const char *pszSubDir /* For recursion. */)
{
    AssertPtrReturn(pContext, VERR_INVALID_POINTER);
    AssertPtrReturn(pszSource, VERR_INVALID_POINTER);
    /* Filter is optional. */
    AssertPtrReturn(pszDest, VERR_INVALID_POINTER);
    /* Sub directory is optional. */

    /*
     * Construct current path.
     */
    char szCurDir[RTPATH_MAX];
    int vrc = RTStrCopy(szCurDir, sizeof(szCurDir), pszSource);
    if (RT_SUCCESS(vrc) && pszSubDir)
        vrc = RTPathAppend(szCurDir, sizeof(szCurDir), pszSubDir);

    if (RT_FAILURE(vrc))
        return vrc;

    if (pContext->fVerbose)
        RTPrintf("Processing guest directory: %s\n", szCurDir);

    /* Flag indicating whether the current directory was created on the
     * target or not. */
    bool fDirCreated = false;
    SafeArray<DirectoryOpenFlag_T> dirOpenFlags; /* No flags supported yet. */
    ComPtr<IGuestDirectory> pDirectory;
    HRESULT rc = pContext->pGuestSession->DirectoryOpen(Bstr(szCurDir).raw(), Bstr(pszFilter).raw(),
                                                        ComSafeArrayAsInParam(dirOpenFlags),
                                                        pDirectory.asOutParam());
    if (FAILED(rc))
        return ctrlPrintError(pContext->pGuestSession, COM_IIDOF(IGuestSession));
    ComPtr<IFsObjInfo> dirEntry;
    while (true)
    {
        rc = pDirectory->Read(dirEntry.asOutParam());
        if (FAILED(rc))
            break;

        FsObjType_T enmType;
        dirEntry->COMGETTER(Type)(&enmType);

        Bstr strName;
        dirEntry->COMGETTER(Name)(strName.asOutParam());

        switch (enmType)
        {
            case FsObjType_Directory:
            {
                Assert(!strName.isEmpty());

                /* Skip "." and ".." entries. */
                if (   !strName.compare(Bstr("."))
                    || !strName.compare(Bstr("..")))
                    break;

                if (pContext->fVerbose)
                {
                    Utf8Str strDir(strName);
                    RTPrintf("Directory: %s\n", strDir.c_str());
                }

                if (fFlags & CopyFileFlag_Recursive)
                {
                    Utf8Str strDir(strName);
                    char *pszNewSub = NULL;
                    if (pszSubDir)
                        pszNewSub = RTPathJoinA(pszSubDir, strDir.c_str());
                    else
                    {
                        pszNewSub = RTStrDup(strDir.c_str());
                        RTPathStripTrailingSlash(pszNewSub);
                    }
                    if (pszNewSub)
                    {
                        vrc = ctrlCopyDirToHost(pContext,
                                                pszSource, pszFilter,
                                                pszDest, fFlags, pszNewSub);
                        RTStrFree(pszNewSub);
                    }
                    else
                        vrc = VERR_NO_MEMORY;
                }
                break;
            }

            case FsObjType_Symlink:
                if (   (fFlags & CopyFileFlag_Recursive)
                    && (fFlags & CopyFileFlag_FollowLinks))
                {
                    /* Fall through to next case is intentional. */
                }
                else
                    break;

            case FsObjType_File:
            {
                Assert(!strName.isEmpty());

                Utf8Str strFile(strName);
                if (   pszFilter
                    && !RTStrSimplePatternMatch(pszFilter, strFile.c_str()))
                {
                    break; /* Filter does not match. */
                }

                if (pContext->fVerbose)
                    RTPrintf("File: %s\n", strFile.c_str());

                if (!fDirCreated)
                {
                    char *pszDestDir;
                    vrc = ctrlCopyTranslatePath(pszSource, szCurDir,
                                                pszDest, &pszDestDir);
                    if (RT_SUCCESS(vrc))
                    {
                        vrc = ctrlCopyDirCreate(pContext, pszDestDir);
                        RTStrFree(pszDestDir);

                        fDirCreated = true;
                    }
                }

                if (RT_SUCCESS(vrc))
                {
                    char *pszFileSource = RTPathJoinA(szCurDir, strFile.c_str());
                    if (pszFileSource)
                    {
                        char *pszFileDest;
                        vrc = ctrlCopyTranslatePath(pszSource, pszFileSource,
                                                   pszDest, &pszFileDest);
                        if (RT_SUCCESS(vrc))
                        {
                            vrc = ctrlCopyFileToDest(pContext, pszFileSource,
                                                    pszFileDest, 0 /* Flags */);
                            RTStrFree(pszFileDest);
                        }
                        RTStrFree(pszFileSource);
                    }
                    else
                        vrc = VERR_NO_MEMORY;
                }
                break;
            }

            default:
                RTPrintf("Warning: Directory entry of type %ld not handled, skipping ...\n",
                         enmType);
                break;
        }

        if (RT_FAILURE(vrc))
            break;
    }

    if (RT_UNLIKELY(FAILED(rc)))
    {
        switch (rc)
        {
            case E_ABORT: /* No more directory entries left to process. */
                break;

            case VBOX_E_FILE_ERROR: /* Current entry cannot be accessed to
                                       to missing rights. */
            {
                RTPrintf("Warning: Cannot access \"%s\", skipping ...\n",
                         szCurDir);
                break;
            }

            default:
                vrc = ctrlPrintError(pDirectory, COM_IIDOF(IGuestDirectory));
                break;
        }
    }

    HRESULT rc2 = pDirectory->Close();
    if (FAILED(rc2))
    {
        int vrc2 = ctrlPrintError(pDirectory, COM_IIDOF(IGuestDirectory));
        if (RT_SUCCESS(vrc))
            vrc = vrc2;
    }
    else if (SUCCEEDED(rc))
        rc = rc2;

    return vrc;
}

/**
 * Copys a directory (tree) to the destination, based on the current copy
 * context.
 *
 * @return  IPRT status code.
 * @param   pContext                Pointer to current copy control context.
 * @param   pszSource               Source directory to copy to the destination.
 * @param   pszFilter               DOS-style wildcard filter (?, *).  Optional.
 * @param   pszDest                 Destination directory where to copy in the source
 *                                  source directory.
 * @param   fFlags                  Copy flags, such as recursive copying.
 */
static int ctrlCopyDirToDest(PCOPYCONTEXT pContext,
                             const char *pszSource, const char *pszFilter,
                             const char *pszDest, uint32_t fFlags)
{
    if (pContext->fHostToGuest)
        return ctrlCopyDirToGuest(pContext, pszSource, pszFilter,
                                  pszDest, fFlags, NULL /* Sub directory, only for recursion. */);
    return ctrlCopyDirToHost(pContext, pszSource, pszFilter,
                             pszDest, fFlags, NULL /* Sub directory, only for recursion. */);
}

/**
 * Creates a source root by stripping file names or filters of the specified source.
 *
 * @return  IPRT status code.
 * @param   pszSource               Source to create source root for.
 * @param   ppszSourceRoot          Pointer that receives the allocated source root. Needs
 *                                  to be free'd with ctrlCopyFreeSourceRoot().
 */
static int ctrlCopyCreateSourceRoot(const char *pszSource, char **ppszSourceRoot)
{
    AssertPtrReturn(pszSource, VERR_INVALID_POINTER);
    AssertPtrReturn(ppszSourceRoot, VERR_INVALID_POINTER);

    char *pszNewRoot = RTStrDup(pszSource);
    AssertPtrReturn(pszNewRoot, VERR_NO_MEMORY);

    size_t lenRoot = strlen(pszNewRoot);
    if (   lenRoot
        && pszNewRoot[lenRoot - 1] == '/'
        && pszNewRoot[lenRoot - 1] == '\\'
        && lenRoot > 1
        && pszNewRoot[lenRoot - 2] == '/'
        && pszNewRoot[lenRoot - 2] == '\\')
    {
        *ppszSourceRoot = pszNewRoot;
        if (lenRoot > 1)
            *ppszSourceRoot[lenRoot - 2] = '\0';
        *ppszSourceRoot[lenRoot - 1] = '\0';
    }
    else
    {
        /* If there's anything (like a file name or a filter),
         * strip it! */
        RTPathStripFilename(pszNewRoot);
        *ppszSourceRoot = pszNewRoot;
    }

    return VINF_SUCCESS;
}

/**
 * Frees a previously allocated source root.
 *
 * @return  IPRT status code.
 * @param   pszSourceRoot           Source root to free.
 */
static void ctrlCopyFreeSourceRoot(char *pszSourceRoot)
{
    RTStrFree(pszSourceRoot);
}

static RTEXITCODE handleCtrlCopy(ComPtr<IGuest> guest, HandlerArg *pArg,
                                 bool fHostToGuest)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    /** @todo r=bird: This command isn't very unix friendly in general. mkdir
     * is much better (partly because it is much simpler of course).  The main
     * arguments against this is that (1) all but two options conflicts with
     * what 'man cp' tells me on a GNU/Linux system, (2) wildchar matching is
     * done windows CMD style (though not in a 100% compatible way), and (3)
     * that only one source is allowed - efficiently sabotaging default
     * wildcard expansion by a unix shell.  The best solution here would be
     * two different variant, one windowsy (xcopy) and one unixy (gnu cp). */

    /*
     * IGuest::CopyToGuest is kept as simple as possible to let the developer choose
     * what and how to implement the file enumeration/recursive lookup, like VBoxManage
     * does in here.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--dryrun",              GETOPTDEF_COPY_DRYRUN,           RTGETOPT_REQ_NOTHING },
        { "--follow",              GETOPTDEF_COPY_FOLLOW,           RTGETOPT_REQ_NOTHING },
        { "--username",            'u',                             RTGETOPT_REQ_STRING  },
        { "--passwordfile",        'p',                             RTGETOPT_REQ_STRING  },
        { "--password",            GETOPTDEF_COPY_PASSWORD,         RTGETOPT_REQ_STRING  },
        { "--domain",              'd',                             RTGETOPT_REQ_STRING  },
        { "--recursive",           'R',                             RTGETOPT_REQ_NOTHING },
        { "--target-directory",    GETOPTDEF_COPY_TARGETDIR,        RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    Utf8Str strSource;
    Utf8Str strDest;
    Utf8Str strUsername;
    Utf8Str strPassword;
    Utf8Str strDomain;
    uint32_t fFlags = CopyFileFlag_None;
    bool fVerbose = false;
    bool fCopyRecursive = false;
    bool fDryRun = false;

    SOURCEVEC vecSources;

    int vrc = VINF_SUCCESS;
    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case GETOPTDEF_COPY_DRYRUN:
                fDryRun = true;
                break;

            case GETOPTDEF_COPY_FOLLOW:
                fFlags |= CopyFileFlag_FollowLinks;
                break;

            case 'u': /* User name */
                strUsername = ValueUnion.psz;
                break;

            case GETOPTDEF_COPY_PASSWORD: /* Password */
                strPassword = ValueUnion.psz;
                break;

            case 'p': /* Password file */
            {
                RTEXITCODE rcExit = readPasswordFile(ValueUnion.psz, &strPassword);
                if (rcExit != RTEXITCODE_SUCCESS)
                    return rcExit;
                break;
            }

            case 'd': /* domain */
                strDomain = ValueUnion.psz;
                break;

            case 'R': /* Recursive processing */
                fFlags |= CopyFileFlag_Recursive;
                break;

            case GETOPTDEF_COPY_TARGETDIR:
                strDest = ValueUnion.psz;
                break;

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                /* Last argument and no destination specified with
                 * --target-directory yet? Then use the current
                 * (= last) argument as destination. */
                if (   pArg->argc == GetState.iNext
                    && strDest.isEmpty())
                {
                    strDest = ValueUnion.psz;
                }
                else
                {
                    /* Save the source directory. */
                    vecSources.push_back(SOURCEFILEENTRY(ValueUnion.psz));
                }
                break;
            }

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    if (!vecSources.size())
        return errorSyntax(USAGE_GUESTCONTROL,
                           "No source(s) specified!");

    if (strDest.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL,
                           "No destination specified!");

    if (strUsername.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL,
                           "No user name specified!");

    /*
     * Done parsing arguments, do some more preparations.
     */
    if (fVerbose)
    {
        if (fHostToGuest)
            RTPrintf("Copying from host to guest ...\n");
        else
            RTPrintf("Copying from guest to host ...\n");
        if (fDryRun)
            RTPrintf("Dry run - no files copied!\n");
    }

    /* Create the copy context -- it contains all information
     * the routines need to know when handling the actual copying. */
    PCOPYCONTEXT pContext = NULL;
    vrc = ctrlCopyContextCreate(guest, fVerbose, fDryRun, fHostToGuest,
                                strUsername, strPassword, strDomain,
                                "VBoxManage Guest Control Copy", &pContext);
    if (RT_FAILURE(vrc))
    {
        RTMsgError("Unable to create copy context, rc=%Rrc\n", vrc);
        return RTEXITCODE_FAILURE;
    }

    /* If the destination is a path, (try to) create it. */
    const char *pszDest = strDest.c_str();
/** @todo r=bird: RTPathFilename and RTPathStripFilename won't work
 * correctly on non-windows hosts when the guest is from the DOS world (Windows,
 * OS/2, DOS).  The host doesn't know about DOS slashes, only UNIX slashes and
 * will get the wrong idea if some dilligent user does:
 *
 *      copyto myfile.txt 'C:\guestfile.txt'
 * or
 *      copyto myfile.txt 'D:guestfile.txt'
 *
 * @bugref{6344}
 */
    if (!RTPathFilename(pszDest))
    {
        vrc = ctrlCopyDirCreate(pContext, pszDest);
    }
    else
    {
        /* We assume we got a file name as destination -- so strip
         * the actual file name and make sure the appropriate
         * directories get created. */
        char *pszDestDir = RTStrDup(pszDest);
        AssertPtr(pszDestDir);
        RTPathStripFilename(pszDestDir);
        vrc = ctrlCopyDirCreate(pContext, pszDestDir);
        RTStrFree(pszDestDir);
    }

    if (RT_SUCCESS(vrc))
    {
        /*
         * Here starts the actual fun!
         * Handle all given sources one by one.
         */
        for (unsigned long s = 0; s < vecSources.size(); s++)
        {
            char *pszSource = RTStrDup(vecSources[s].GetSource());
            AssertPtrBreakStmt(pszSource, vrc = VERR_NO_MEMORY);
            const char *pszFilter = vecSources[s].GetFilter();
            if (!strlen(pszFilter))
                pszFilter = NULL; /* If empty filter then there's no filter :-) */

            char *pszSourceRoot;
            vrc = ctrlCopyCreateSourceRoot(pszSource, &pszSourceRoot);
            if (RT_FAILURE(vrc))
            {
                RTMsgError("Unable to create source root, rc=%Rrc\n", vrc);
                break;
            }

            if (fVerbose)
                RTPrintf("Source: %s\n", pszSource);

            /** @todo Files with filter?? */
            bool fSourceIsFile = false;
            bool fSourceExists;

            size_t cchSource = strlen(pszSource);
            if (   cchSource > 1
                && RTPATH_IS_SLASH(pszSource[cchSource - 1]))
            {
                if (pszFilter) /* Directory with filter (so use source root w/o the actual filter). */
                    vrc = ctrlCopyDirExistsOnSource(pContext, pszSourceRoot, &fSourceExists);
                else /* Regular directory without filter. */
                    vrc = ctrlCopyDirExistsOnSource(pContext, pszSource, &fSourceExists);

                if (fSourceExists)
                {
                    /* Strip trailing slash from our source element so that other functions
                     * can use this stuff properly (like RTPathStartsWith). */
                    RTPathStripTrailingSlash(pszSource);
                }
            }
            else
            {
                vrc = ctrlCopyFileExistsOnSource(pContext, pszSource, &fSourceExists);
                if (   RT_SUCCESS(vrc)
                    && fSourceExists)
                {
                    fSourceIsFile = true;
                }
            }

            if (   RT_SUCCESS(vrc)
                && fSourceExists)
            {
                if (fSourceIsFile)
                {
                    /* Single file. */
                    char *pszDestFile;
                    vrc = ctrlCopyTranslatePath(pszSourceRoot, pszSource,
                                                strDest.c_str(), &pszDestFile);
                    if (RT_SUCCESS(vrc))
                    {
                        vrc = ctrlCopyFileToDest(pContext, pszSource,
                                                 pszDestFile, 0 /* Flags */);
                        RTStrFree(pszDestFile);
                    }
                    else
                        RTMsgError("Unable to translate path for \"%s\", rc=%Rrc\n",
                                   pszSource, vrc);
                }
                else
                {
                    /* Directory (with filter?). */
                    vrc = ctrlCopyDirToDest(pContext, pszSource, pszFilter,
                                            strDest.c_str(), fFlags);
                }
            }

            ctrlCopyFreeSourceRoot(pszSourceRoot);

            if (   RT_SUCCESS(vrc)
                && !fSourceExists)
            {
                RTMsgError("Warning: Source \"%s\" does not exist, skipping!\n",
                           pszSource);
                RTStrFree(pszSource);
                continue;
            }
            else if (RT_FAILURE(vrc))
            {
                RTMsgError("Error processing \"%s\", rc=%Rrc\n",
                           pszSource, vrc);
                RTStrFree(pszSource);
                break;
            }

            RTStrFree(pszSource);
        }
    }

    ctrlCopyContextFree(pContext);

    return RT_SUCCESS(vrc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

static RTEXITCODE handleCtrlCreateDirectory(ComPtr<IGuest> pGuest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    /*
     * Parse arguments.
     *
     * Note! No direct returns here, everyone must go thru the cleanup at the
     *       end of this function.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--mode",                'm',                             RTGETOPT_REQ_UINT32  },
        { "--parents",             'P',                             RTGETOPT_REQ_NOTHING },
        { "--username",            'u',                             RTGETOPT_REQ_STRING  },
        { "--passwordfile",        'p',                             RTGETOPT_REQ_STRING  },
        { "--password",            GETOPTDEF_MKDIR_PASSWORD,        RTGETOPT_REQ_STRING  },
        { "--domain",              'd',                             RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    Utf8Str strUsername;
    Utf8Str strPassword;
    Utf8Str strDomain;
    SafeArray<DirectoryCreateFlag_T> dirCreateFlags;
    uint32_t fDirMode = 0; /* Default mode. */
    bool fVerbose = false;

    DESTDIRMAP mapDirs;

    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'm': /* Mode */
                fDirMode = ValueUnion.u32;
                break;

            case 'P': /* Create parents */
                dirCreateFlags.push_back(DirectoryCreateFlag_Parents);
                break;

            case 'u': /* User name */
                strUsername = ValueUnion.psz;
                break;

            case GETOPTDEF_MKDIR_PASSWORD: /* Password */
                strPassword = ValueUnion.psz;
                break;

            case 'p': /* Password file */
            {
                RTEXITCODE rcExit = readPasswordFile(ValueUnion.psz, &strPassword);
                if (rcExit != RTEXITCODE_SUCCESS)
                    return rcExit;
                break;
            }

            case 'd': /* domain */
                strDomain = ValueUnion.psz;
                break;

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                mapDirs[ValueUnion.psz]; /* Add destination directory to map. */
                break;
            }

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    uint32_t cDirs = mapDirs.size();
    if (!cDirs)
        return errorSyntax(USAGE_GUESTCONTROL, "No directory to create specified!");

    if (strUsername.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No user name specified!");

    /*
     * Create the directories.
     */
    HRESULT hrc = S_OK;
    if (fVerbose && cDirs)
        RTPrintf("Creating %u directories ...\n", cDirs);

    ComPtr<IGuestSession> pGuestSession;
    hrc = pGuest->CreateSession(Bstr(strUsername).raw(),
                                Bstr(strPassword).raw(),
                                Bstr(strDomain).raw(),
                                Bstr("VBoxManage Guest Control MkDir").raw(),
                                pGuestSession.asOutParam());
    if (FAILED(hrc))
    {
        ctrlPrintError(pGuest, COM_IIDOF(IGuest));
        return RTEXITCODE_FAILURE;
    }

    DESTDIRMAPITER it = mapDirs.begin();
    while (it != mapDirs.end())
    {
        if (fVerbose)
            RTPrintf("Creating directory \"%s\" ...\n", it->first.c_str());

        hrc = pGuestSession->DirectoryCreate(Bstr(it->first).raw(), fDirMode, ComSafeArrayAsInParam(dirCreateFlags));
        if (FAILED(hrc))
        {
            ctrlPrintError(pGuest, COM_IIDOF(IGuestSession)); /* Return code ignored, save original rc. */
            break;
        }

        it++;
    }

    if (!pGuestSession.isNull())
        pGuestSession->Close();

    return FAILED(hrc) ? RTEXITCODE_FAILURE : RTEXITCODE_SUCCESS;
}

static RTEXITCODE handleCtrlCreateTemp(ComPtr<IGuest> pGuest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    /*
     * Parse arguments.
     *
     * Note! No direct returns here, everyone must go thru the cleanup at the
     *       end of this function.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--mode",                'm',                             RTGETOPT_REQ_UINT32  },
        { "--directory",           'D',                             RTGETOPT_REQ_NOTHING },
        { "--secure",              's',                             RTGETOPT_REQ_NOTHING },
        { "--tmpdir",              't',                             RTGETOPT_REQ_STRING  },
        { "--username",            'u',                             RTGETOPT_REQ_STRING  },
        { "--passwordfile",        'p',                             RTGETOPT_REQ_STRING  },
        { "--password",            GETOPTDEF_MKDIR_PASSWORD,        RTGETOPT_REQ_STRING  },
        { "--domain",              'd',                             RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    Utf8Str strUsername;
    Utf8Str strPassword;
    Utf8Str strDomain;
    Utf8Str strTemplate;
    uint32_t fMode = 0; /* Default mode. */
    bool fDirectory = false;
    bool fSecure = false;
    Utf8Str strTempDir;
    bool fVerbose = false;

    DESTDIRMAP mapDirs;

    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'm': /* Mode */
                fMode = ValueUnion.u32;
                break;

            case 'D': /* Create directory */
                fDirectory = true;
                break;

            case 's': /* Secure */
                fSecure = true;
                break;

            case 't': /* Temp directory */
                strTempDir = ValueUnion.psz;
                break;

            case 'u': /* User name */
                strUsername = ValueUnion.psz;
                break;

            case GETOPTDEF_MKDIR_PASSWORD: /* Password */
                strPassword = ValueUnion.psz;
                break;

            case 'p': /* Password file */
            {
                RTEXITCODE rcExit = readPasswordFile(ValueUnion.psz, &strPassword);
                if (rcExit != RTEXITCODE_SUCCESS)
                    return rcExit;
                break;
            }

            case 'd': /* domain */
                strDomain = ValueUnion.psz;
                break;

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                if (strTemplate.isEmpty())
                    strTemplate = ValueUnion.psz;
                else
                    return errorSyntax(USAGE_GUESTCONTROL,
                                       "More than one template specified!\n");
                break;
            }

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    if (strTemplate.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No template specified!");

    if (strUsername.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No user name specified!");

    if (!fDirectory)
        return errorSyntax(USAGE_GUESTCONTROL, "Creating temporary files is currently not supported!");

    /*
     * Create the directories.
     */
    HRESULT hrc = S_OK;
    if (fVerbose)
    {
        if (fDirectory && !strTempDir.isEmpty())
            RTPrintf("Creating temporary directory from template '%s' in directory '%s' ...\n",
                     strTemplate.c_str(), strTempDir.c_str());
        else if (fDirectory)
            RTPrintf("Creating temporary directory from template '%s' in default temporary directory ...\n",
                     strTemplate.c_str());
        else if (!fDirectory && !strTempDir.isEmpty())
            RTPrintf("Creating temporary file from template '%s' in directory '%s' ...\n",
                     strTemplate.c_str(), strTempDir.c_str());
        else if (!fDirectory)
            RTPrintf("Creating temporary file from template '%s' in default temporary directory ...\n",
                     strTemplate.c_str());
    }

    ComPtr<IGuestSession> pGuestSession;
    hrc = pGuest->CreateSession(Bstr(strUsername).raw(),
                                Bstr(strPassword).raw(),
                                Bstr(strDomain).raw(),
                                Bstr("VBoxManage Guest Control MkTemp").raw(),
                                pGuestSession.asOutParam());
    if (FAILED(hrc))
    {
        ctrlPrintError(pGuest, COM_IIDOF(IGuest));
        return RTEXITCODE_FAILURE;
    }

    if (fDirectory)
    {
        Bstr directory;
        hrc = pGuestSession->DirectoryCreateTemp(Bstr(strTemplate).raw(),
                                                 fMode, Bstr(strTempDir).raw(),
                                                 fSecure,
                                                 directory.asOutParam());
        if (SUCCEEDED(hrc))
            RTPrintf("Directory name: %ls\n", directory.raw());
    }
    // else - temporary file not yet implemented
    if (FAILED(hrc))
        ctrlPrintError(pGuest, COM_IIDOF(IGuestSession)); /* Return code ignored, save original rc. */

    if (!pGuestSession.isNull())
        pGuestSession->Close();

    return FAILED(hrc) ? RTEXITCODE_FAILURE : RTEXITCODE_SUCCESS;
}

static int handleCtrlStat(ComPtr<IGuest> pGuest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, VERR_INVALID_PARAMETER);

    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--dereference",         'L',                             RTGETOPT_REQ_NOTHING },
        { "--file-system",         'f',                             RTGETOPT_REQ_NOTHING },
        { "--format",              'c',                             RTGETOPT_REQ_STRING },
        { "--username",            'u',                             RTGETOPT_REQ_STRING  },
        { "--passwordfile",        'p',                             RTGETOPT_REQ_STRING  },
        { "--password",            GETOPTDEF_STAT_PASSWORD,         RTGETOPT_REQ_STRING  },
        { "--domain",              'd',                             RTGETOPT_REQ_STRING  },
        { "--terse",               't',                             RTGETOPT_REQ_NOTHING },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    Utf8Str strUsername;
    Utf8Str strPassword;
    Utf8Str strDomain;

    bool fVerbose = false;
    DESTDIRMAP mapObjs;

    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'u': /* User name */
                strUsername = ValueUnion.psz;
                break;

            case GETOPTDEF_STAT_PASSWORD: /* Password */
                strPassword = ValueUnion.psz;
                break;

            case 'p': /* Password file */
            {
                RTEXITCODE rcExit = readPasswordFile(ValueUnion.psz, &strPassword);
                if (rcExit != RTEXITCODE_SUCCESS)
                    return rcExit;
                break;
            }

            case 'd': /* domain */
                strDomain = ValueUnion.psz;
                break;

            case 'L': /* Dereference */
            case 'f': /* File-system */
            case 'c': /* Format */
            case 't': /* Terse */
                return errorSyntax(USAGE_GUESTCONTROL, "Command \"%s\" not implemented yet!",
                                   ValueUnion.psz);
                break; /* Never reached. */

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                mapObjs[ValueUnion.psz]; /* Add element to check to map. */
                break;
            }

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    uint32_t cObjs = mapObjs.size();
    if (!cObjs)
        return errorSyntax(USAGE_GUESTCONTROL, "No element(s) to check specified!");

    if (strUsername.isEmpty())
        return errorSyntax(USAGE_GUESTCONTROL, "No user name specified!");

    ComPtr<IGuestSession> pGuestSession;
    HRESULT hrc = pGuest->CreateSession(Bstr(strUsername).raw(),
                                        Bstr(strPassword).raw(),
                                        Bstr(strDomain).raw(),
                                        Bstr("VBoxManage Guest Control Stat").raw(),
                                        pGuestSession.asOutParam());
    if (FAILED(hrc))
        return ctrlPrintError(pGuest, COM_IIDOF(IGuest));

    /*
     * Create the directories.
     */
    RTEXITCODE rcExit = RTEXITCODE_SUCCESS;
    DESTDIRMAPITER it = mapObjs.begin();
    while (it != mapObjs.end())
    {
        if (fVerbose)
            RTPrintf("Checking for element \"%s\" ...\n", it->first.c_str());

        ComPtr<IGuestFsObjInfo> pFsObjInfo;
        hrc = pGuestSession->FileQueryInfo(Bstr(it->first).raw(), pFsObjInfo.asOutParam());
        if (FAILED(hrc))
            hrc = pGuestSession->DirectoryQueryInfo(Bstr(it->first).raw(), pFsObjInfo.asOutParam());

        if (FAILED(hrc))
        {
            /* If there's at least one element which does not exist on the guest,
             * drop out with exitcode 1. */
            if (fVerbose)
                RTPrintf("Cannot stat for element \"%s\": No such element\n",
                         it->first.c_str());
            rcExit = RTEXITCODE_FAILURE;
        }
        else
        {
            FsObjType_T objType;
            hrc = pFsObjInfo->COMGETTER(Type)(&objType);
            if (FAILED(hrc))
                return ctrlPrintError(pGuest, COM_IIDOF(IGuestFsObjInfo));
            switch (objType)
            {
                case FsObjType_File:
                    RTPrintf("Element \"%s\" found: Is a file\n", it->first.c_str());
                    break;

                case FsObjType_Directory:
                    RTPrintf("Element \"%s\" found: Is a directory\n", it->first.c_str());
                    break;

                case FsObjType_Symlink:
                    RTPrintf("Element \"%s\" found: Is a symlink\n", it->first.c_str());
                    break;

                default:
                    RTPrintf("Element \"%s\" found, type unknown (%ld)\n", it->first.c_str(), objType);
                    break;
            }

            /** @todo: Show more information about this element. */
        }

        it++;
    }

    if (!pGuestSession.isNull())
        pGuestSession->Close();

    return rcExit;
}

static int handleCtrlUpdateAdditions(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, VERR_INVALID_PARAMETER);

    /*
     * Check the syntax.  We can deduce the correct syntax from the number of
     * arguments.
     */
    Utf8Str strSource;
    com::SafeArray<IN_BSTR> aArgs;
    bool fVerbose = false;
    bool fWaitStartOnly = false;

    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--source",              's',         RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',         RTGETOPT_REQ_NOTHING },
        { "--wait-start",          'w',         RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv, s_aOptions, RT_ELEMENTS(s_aOptions), 0, 0);

    int vrc = VINF_SUCCESS;
    while (   (ch = RTGetOpt(&GetState, &ValueUnion))
           && RT_SUCCESS(vrc))
    {
        switch (ch)
        {
            case 's':
                strSource = ValueUnion.psz;
                break;

            case 'v':
                fVerbose = true;
                break;

            case 'w':
                fWaitStartOnly = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                if (aArgs.size() == 0 && strSource.isEmpty())
                    strSource = ValueUnion.psz;
                else
                    aArgs.push_back(Bstr(ValueUnion.psz).raw());
                break;
            }

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    if (fVerbose)
        RTPrintf("Updating Guest Additions ...\n");

    HRESULT rc = S_OK;
    while (strSource.isEmpty())
    {
        ComPtr<ISystemProperties> pProperties;
        CHECK_ERROR_BREAK(pArg->virtualBox, COMGETTER(SystemProperties)(pProperties.asOutParam()));
        Bstr strISO;
        CHECK_ERROR_BREAK(pProperties, COMGETTER(DefaultAdditionsISO)(strISO.asOutParam()));
        strSource = strISO;
        break;
    }

    /* Determine source if not set yet. */
    if (strSource.isEmpty())
    {
        RTMsgError("No Guest Additions source found or specified, aborting\n");
        vrc = VERR_FILE_NOT_FOUND;
    }
    else if (!RTFileExists(strSource.c_str()))
    {
        RTMsgError("Source \"%s\" does not exist!\n", strSource.c_str());
        vrc = VERR_FILE_NOT_FOUND;
    }

    if (RT_SUCCESS(vrc))
    {
        if (fVerbose)
            RTPrintf("Using source: %s\n", strSource.c_str());

        com::SafeArray<AdditionsUpdateFlag_T> aUpdateFlags;
        if (fWaitStartOnly)
        {
            aUpdateFlags.push_back(AdditionsUpdateFlag_WaitForUpdateStartOnly);
            if (fVerbose)
                RTPrintf("Preparing and waiting for Guest Additions installer to start ...\n");
        }

        ComPtr<IProgress> pProgress;
        CHECK_ERROR(guest, UpdateGuestAdditions(Bstr(strSource).raw(),
                                                ComSafeArrayAsInParam(aArgs),
                                                /* Wait for whole update process to complete. */
                                                ComSafeArrayAsInParam(aUpdateFlags),
                                                pProgress.asOutParam()));
        if (FAILED(rc))
            vrc = ctrlPrintError(guest, COM_IIDOF(IGuest));
        else
        {
            if (fVerbose)
                rc = showProgress(pProgress);
            else
                rc = pProgress->WaitForCompletion(-1 /* No timeout */);

            if (SUCCEEDED(rc))
                CHECK_PROGRESS_ERROR(pProgress, ("Guest additions update failed"));
            vrc = ctrlPrintProgressError(pProgress);
            if (   RT_SUCCESS(vrc)
                && fVerbose)
            {
                RTPrintf("Guest Additions update successful\n");
            }
        }
    }

    return RT_SUCCESS(vrc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

static RTEXITCODE handleCtrlList(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    if (pArg->argc < 1)
        return errorSyntax(USAGE_GUESTCONTROL, "Must specify a listing category");

    RTEXITCODE rcExit = RTEXITCODE_SUCCESS;

    /** Use RTGetOpt here when handling command line args gets more complex. */

    bool fListAll = false;
    bool fListSessions = false;
    bool fListProcesses = false;
    if (   !RTStrICmp(pArg->argv[0], "sessions")
        || !RTStrICmp(pArg->argv[0], "sess"))
        fListSessions = true;
    else if (   !RTStrICmp(pArg->argv[0], "processes")
             || !RTStrICmp(pArg->argv[0], "procs"))
        fListSessions = fListProcesses = true; /* Showing processes implies showing sessions. */
    else if (!RTStrICmp(pArg->argv[0], "all"))
        fListAll = true;

    /** @todo Handle "--verbose" using RTGetOpt. */
    /** @todo Do we need a machine-readable output here as well? */

    if (   fListAll
        || fListSessions)
    {
        HRESULT rc;
        do
        {
            size_t cTotalProcs = 0;

            SafeIfaceArray <IGuestSession> collSessions;
            CHECK_ERROR_BREAK(guest, COMGETTER(Sessions)(ComSafeArrayAsOutParam(collSessions)));
            size_t cSessions = collSessions.size();

            if (cSessions)
            {
                RTPrintf("Active guest sessions:\n");

                /** @todo Make this output a bit prettier. No time now. */

                for (size_t i = 0; i < cSessions; i++)
                {
                    ComPtr<IGuestSession> pCurSession = collSessions[i];
                    if (!pCurSession.isNull())
                    {
                        ULONG uID;
                        CHECK_ERROR_BREAK(pCurSession, COMGETTER(Id)(&uID));
                        Bstr strName;
                        CHECK_ERROR_BREAK(pCurSession, COMGETTER(Name)(strName.asOutParam()));
                        Bstr strUser;
                        CHECK_ERROR_BREAK(pCurSession, COMGETTER(User)(strUser.asOutParam()));
                        GuestSessionStatus_T sessionStatus;
                        CHECK_ERROR_BREAK(pCurSession, COMGETTER(Status)(&sessionStatus));
                        RTPrintf("\n\tSession #%-3zu ID=%-3RU32 User=%-16ls Status=[%s] Name=%ls",
                                 i, uID, strUser.raw(), ctrlSessionStatusToText(sessionStatus), strName.raw());

                        if (   fListAll
                            || fListProcesses)
                        {
                            SafeIfaceArray <IGuestProcess> collProcesses;
                            CHECK_ERROR_BREAK(pCurSession, COMGETTER(Processes)(ComSafeArrayAsOutParam(collProcesses)));
                            for (size_t a = 0; a < collProcesses.size(); a++)
                            {
                                ComPtr<IGuestProcess> pCurProcess = collProcesses[a];
                                if (!pCurProcess.isNull())
                                {
                                    ULONG uPID;
                                    CHECK_ERROR_BREAK(pCurProcess, COMGETTER(PID)(&uPID));
                                    Bstr strExecPath;
                                    CHECK_ERROR_BREAK(pCurProcess, COMGETTER(ExecutablePath)(strExecPath.asOutParam()));
                                    ProcessStatus_T procStatus;
                                    CHECK_ERROR_BREAK(pCurProcess, COMGETTER(Status)(&procStatus));

                                    RTPrintf("\n\t\tProcess #%-03zu PID=%-6RU32 Status=[%s] Command=%ls",
                                             a, uPID, ctrlExecProcessStatusToText(procStatus), strExecPath.raw());
                                }
                            }

                            cTotalProcs += collProcesses.size();
                        }
                    }
                }

                RTPrintf("\n\nTotal guest sessions: %zu\n", collSessions.size());
                RTPrintf("Total guest processes: %zu\n", cTotalProcs);
            }
            else
                RTPrintf("No active guest sessions found\n");

        } while (0);

        if (FAILED(rc))
            rcExit = RTEXITCODE_FAILURE;
    }
    else
        return errorSyntax(USAGE_GUESTCONTROL, "Invalid listing category '%s", pArg->argv[0]);

    return rcExit;
}

static RTEXITCODE handleCtrlProcessClose(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    if (pArg->argc < 1)
        return errorSyntax(USAGE_GUESTCONTROL, "Must specify at least a PID to close");

    /*
     * Parse arguments.
     *
     * Note! No direct returns here, everyone must go thru the cleanup at the
     *       end of this function.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--session-id",          'i',                             RTGETOPT_REQ_UINT32  },
        { "--session-name",        'n',                             RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    std::vector < uint32_t > vecPID;
    ULONG ulSessionID = UINT32_MAX;
    Utf8Str strSessionName;
    bool fVerbose = false;

    int vrc = VINF_SUCCESS;

    while (   (ch = RTGetOpt(&GetState, &ValueUnion))
           && RT_SUCCESS(vrc))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'n': /* Session name (or pattern) */
                strSessionName = ValueUnion.psz;
                break;

            case 'i': /* Session ID */
                ulSessionID = ValueUnion.u32;
                break;

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
                if (pArg->argc == GetState.iNext)
                {
                    /* Treat every else specified as a PID to kill. */
                    try
                    {
                        uint32_t uPID = RTStrToUInt32(ValueUnion.psz);
                        if (uPID) /** @todo Is this what we want? If specifying PID 0
                                            this is not going to work on most systems anyway. */
                            vecPID.push_back(uPID);
                        else
                            vrc = VERR_INVALID_PARAMETER;
                    }
                    catch(std::bad_alloc &)
                    {
                        vrc = VERR_NO_MEMORY;
                    }
                }
                break;

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    if (vecPID.empty())
        return errorSyntax(USAGE_GUESTCONTROL, "At least one PID must be specified to kill!");
    else if (   strSessionName.isEmpty()
             && ulSessionID == UINT32_MAX)
    {
        return errorSyntax(USAGE_GUESTCONTROL, "No session ID specified!");
    }
    else if (   !strSessionName.isEmpty()
             && ulSessionID != UINT32_MAX)
    {
        return errorSyntax(USAGE_GUESTCONTROL, "Either session ID or name (pattern) must be specified");
    }

    if (RT_FAILURE(vrc))
        return errorSyntax(USAGE_GUESTCONTROL, "Invalid parameters specified");

    HRESULT rc = S_OK;

    ComPtr<IGuestSession> pSession;
    ComPtr<IGuestProcess> pProcess;
    do
    {
        uint32_t uProcsTerminated = 0;
        bool fSessionFound = false;

        SafeIfaceArray <IGuestSession> collSessions;
        CHECK_ERROR_BREAK(guest, COMGETTER(Sessions)(ComSafeArrayAsOutParam(collSessions)));
        size_t cSessions = collSessions.size();

        uint32_t uSessionsHandled = 0;
        for (size_t i = 0; i < cSessions; i++)
        {
            pSession = collSessions[i];
            Assert(!pSession.isNull());

            ULONG uID; /* Session ID */
            CHECK_ERROR_BREAK(pSession, COMGETTER(Id)(&uID));
            Bstr strName;
            CHECK_ERROR_BREAK(pSession, COMGETTER(Name)(strName.asOutParam()));
            Utf8Str strNameUtf8(strName); /* Session name */
            if (strSessionName.isEmpty()) /* Search by ID. Slow lookup. */
            {
                fSessionFound = uID == ulSessionID;
            }
            else /* ... or by naming pattern. */
            {
                if (RTStrSimplePatternMatch(strSessionName.c_str(), strNameUtf8.c_str()))
                    fSessionFound = true;
            }

            if (fSessionFound)
            {
                AssertStmt(!pSession.isNull(), break);
                uSessionsHandled++;

                SafeIfaceArray <IGuestProcess> collProcs;
                CHECK_ERROR_BREAK(pSession, COMGETTER(Processes)(ComSafeArrayAsOutParam(collProcs)));

                size_t cProcs = collProcs.size();
                for (size_t p = 0; p < cProcs; p++)
                {
                    pProcess = collProcs[p];
                    Assert(!pProcess.isNull());

                    ULONG uPID; /* Process ID */
                    CHECK_ERROR_BREAK(pProcess, COMGETTER(PID)(&uPID));

                    bool fProcFound = false;
                    for (size_t a = 0; a < vecPID.size(); a++) /* Slow, but works. */
                    {
                        fProcFound = vecPID[a] == uPID;
                        if (fProcFound)
                            break;
                    }

                    if (fProcFound)
                    {
                        if (fVerbose)
                            RTPrintf("Terminating process (PID %RU32) (session ID %RU32) ...\n",
                                     uPID, uID);
                        CHECK_ERROR_BREAK(pProcess, Terminate());
                        uProcsTerminated++;
                    }
                    else
                    {
                        if (ulSessionID != UINT32_MAX)
                            RTPrintf("No matching process(es) for session ID %RU32 found\n",
                                     ulSessionID);
                    }

                    pProcess.setNull();
                }

                pSession.setNull();
            }
        }

        if (!uSessionsHandled)
            RTPrintf("No matching session(s) found\n");

        if (uProcsTerminated)
            RTPrintf("%RU32 %s terminated\n",
                     uProcsTerminated, uProcsTerminated == 1 ? "process" : "processes");

    } while (0);

    pProcess.setNull();
    pSession.setNull();

    return SUCCEEDED(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

static RTEXITCODE handleCtrlProcess(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    if (pArg->argc < 1)
        return errorSyntax(USAGE_GUESTCONTROL, "Must specify an action");

    /** Use RTGetOpt here when handling command line args gets more complex. */

    HandlerArg argSub = *pArg;
    argSub.argc = pArg->argc - 1; /* Skip session action. */
    argSub.argv = pArg->argv + 1; /* Same here. */

    if (   !RTStrICmp(pArg->argv[0], "close")
        || !RTStrICmp(pArg->argv[0], "kill")
        || !RTStrICmp(pArg->argv[0], "terminate"))
    {
        return handleCtrlProcessClose(guest, &argSub);
    }

    return errorSyntax(USAGE_GUESTCONTROL, "Invalid process action '%s'", pArg->argv[0]);
}

static RTEXITCODE handleCtrlSessionClose(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    if (pArg->argc < 1)
        return errorSyntax(USAGE_GUESTCONTROL, "Must specify at least a session ID to close");

    /*
     * Parse arguments.
     *
     * Note! No direct returns here, everyone must go thru the cleanup at the
     *       end of this function.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--all",                 GETOPTDEF_SESSIONCLOSE_ALL,      RTGETOPT_REQ_NOTHING  },
        { "--session-id",          'i',                             RTGETOPT_REQ_UINT32  },
        { "--session-name",        'n',                             RTGETOPT_REQ_STRING  },
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    ULONG ulSessionID = UINT32_MAX;
    Utf8Str strSessionName;
    bool fVerbose = false;

    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'n': /* Session name pattern */
                strSessionName = ValueUnion.psz;
                break;

            case 'i': /* Session ID */
                ulSessionID = ValueUnion.u32;
                break;

            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case GETOPTDEF_SESSIONCLOSE_ALL:
                strSessionName = "*";
                break;

            case VINF_GETOPT_NOT_OPTION:
                /** @todo Supply a CSV list of IDs or patterns to close? */
                break;

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    if (   strSessionName.isEmpty()
        && ulSessionID == UINT32_MAX)
    {
        return errorSyntax(USAGE_GUESTCONTROL, "No session ID specified!");
    }
    else if (   !strSessionName.isEmpty()
             && ulSessionID != UINT32_MAX)
    {
        return errorSyntax(USAGE_GUESTCONTROL, "Either session ID or name (pattern) must be specified");
    }

    HRESULT rc = S_OK;

    ComPtr<IGuestSession> pSession;
    do
    {
        bool fSessionFound = false;
        size_t cSessionsHandled = 0;

        SafeIfaceArray <IGuestSession> collSessions;
        CHECK_ERROR_BREAK(guest, COMGETTER(Sessions)(ComSafeArrayAsOutParam(collSessions)));
        size_t cSessions = collSessions.size();

        for (size_t i = 0; i < cSessions; i++)
        {
            pSession = collSessions[i];
            Assert(!pSession.isNull());

            ULONG uID; /* Session ID */
            CHECK_ERROR_BREAK(pSession, COMGETTER(Id)(&uID));
            Bstr strName;
            CHECK_ERROR_BREAK(pSession, COMGETTER(Name)(strName.asOutParam()));
            Utf8Str strNameUtf8(strName); /* Session name */

            if (strSessionName.isEmpty()) /* Search by ID. Slow lookup. */
            {
                fSessionFound = uID == ulSessionID;
            }
            else /* ... or by naming pattern. */
            {
                if (RTStrSimplePatternMatch(strSessionName.c_str(), strNameUtf8.c_str()))
                    fSessionFound = true;
            }

            if (fSessionFound)
            {
                cSessionsHandled++;

                Assert(!pSession.isNull());
                if (fVerbose)
                    RTPrintf("Closing guest session ID=#%RU32 \"%s\" ...\n",
                             uID, strNameUtf8.c_str());
                CHECK_ERROR_BREAK(pSession, Close());
                if (fVerbose)
                    RTPrintf("Guest session successfully closed\n");

                pSession->Release();
            }
        }

        if (!cSessionsHandled)
        {
            RTPrintf("No guest session(s) found\n");
            rc = E_ABORT; /* To set exit code accordingly. */
        }

    } while (0);

    return SUCCEEDED(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

static RTEXITCODE handleCtrlSession(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    if (pArg->argc < 1)
        return errorSyntax(USAGE_GUESTCONTROL, "Must specify an action");

    /** Use RTGetOpt here when handling command line args gets more complex. */

    HandlerArg argSub = *pArg;
    argSub.argc = pArg->argc - 1; /* Skip session action. */
    argSub.argv = pArg->argv + 1; /* Same here. */

    if (   !RTStrICmp(pArg->argv[0], "close")
        || !RTStrICmp(pArg->argv[0], "kill")
        || !RTStrICmp(pArg->argv[0], "terminate"))
    {
        return handleCtrlSessionClose(guest, &argSub);
    }

    return errorSyntax(USAGE_GUESTCONTROL, "Invalid session action '%s'", pArg->argv[0]);
}

static RTEXITCODE handleCtrlWatch(ComPtr<IGuest> guest, HandlerArg *pArg)
{
    AssertPtrReturn(pArg, RTEXITCODE_SYNTAX);

    /*
     * Parse arguments.
     */
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--verbose",             'v',                             RTGETOPT_REQ_NOTHING }
    };

    int ch;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, pArg->argc, pArg->argv,
                 s_aOptions, RT_ELEMENTS(s_aOptions), 0, RTGETOPTINIT_FLAGS_OPTS_FIRST);

    bool fVerbose = false;

    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        /* For options that require an argument, ValueUnion has received the value. */
        switch (ch)
        {
            case 'v': /* Verbose */
                fVerbose = true;
                break;

            case VINF_GETOPT_NOT_OPTION:
                break;

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    /** @todo Specify categories to watch for. */
    /** @todo Specify a --timeout for waiting only for a certain amount of time? */

    HRESULT rc;

    try
    {
        ComObjPtr<GuestEventListenerImpl> pGuestListener;



        do
        {
            /* Listener creation. */
            pGuestListener.createObject();
            pGuestListener->init(new GuestEventListener());

            /* Register for IGuest events. */
            ComPtr<IEventSource> es;
            CHECK_ERROR_BREAK(guest, COMGETTER(EventSource)(es.asOutParam()));
            com::SafeArray<VBoxEventType_T> eventTypes;
            eventTypes.push_back(VBoxEventType_OnGuestSessionRegistered);
            /** @todo Also register for VBoxEventType_OnGuestUserStateChanged on demand? */
            CHECK_ERROR_BREAK(es, RegisterListener(pGuestListener, ComSafeArrayAsInParam(eventTypes),
                                                   true /* Active listener */));
            /* Note: All other guest control events have to be registered
             *       as their corresponding objects appear. */

        } while (0);

        ctrlSignalHandlerInstall();

        if (fVerbose)
            RTPrintf("Waiting for events ...\n");

        while (!g_fGuestCtrlCanceled)
        {
            /** @todo Timeout handling (see above)? */
            RTThreadYield();
        }

        if (fVerbose)
            RTPrintf("Signal caught, exiting ...\n");

        ctrlSignalHandlerUninstall();

        if (!pGuestListener.isNull())
        {
            /* Guest callback unregistration. */
            ComPtr<IEventSource> pES;
            CHECK_ERROR(guest, COMGETTER(EventSource)(pES.asOutParam()));
            if (!pES.isNull())
                CHECK_ERROR(pES, UnregisterListener(pGuestListener));
            pGuestListener.setNull();
        }
    }
    catch (std::bad_alloc &)
    {
        rc = E_OUTOFMEMORY;
    }

    return SUCCEEDED(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

/**
 * Access the guest control store.
 *
 * @returns program exit code.
 * @note see the command line API description for parameters
 */
int handleGuestControl(HandlerArg *pArg)
{
    AssertPtrReturn(pArg, VERR_INVALID_PARAMETER);

#ifdef DEBUG_andy_disabled
    if (RT_FAILURE(tstTranslatePath()))
        return RTEXITCODE_FAILURE;
#endif

    HandlerArg arg = *pArg;
    arg.argc = pArg->argc - 2; /* Skip VM name and sub command. */
    arg.argv = pArg->argv + 2; /* Same here. */

    ComPtr<IGuest> guest;
    int vrc = ctrlInitVM(pArg, pArg->argv[0] /* VM Name */, &guest);
    if (RT_SUCCESS(vrc))
    {
        int rcExit;
        if (pArg->argc < 2)
            rcExit = errorSyntax(USAGE_GUESTCONTROL, "No sub command specified!");
        else if (   !RTStrICmp(pArg->argv[1], "exec")
                 || !RTStrICmp(pArg->argv[1], "execute"))
            rcExit = handleCtrlProcessExec(guest, &arg);
        else if (!RTStrICmp(pArg->argv[1], "copyfrom"))
            rcExit = handleCtrlCopy(guest, &arg, false /* Guest to host */);
        else if (   !RTStrICmp(pArg->argv[1], "copyto")
                 || !RTStrICmp(pArg->argv[1], "cp"))
            rcExit = handleCtrlCopy(guest, &arg, true /* Host to guest */);
        else if (   !RTStrICmp(pArg->argv[1], "createdirectory")
                 || !RTStrICmp(pArg->argv[1], "createdir")
                 || !RTStrICmp(pArg->argv[1], "mkdir")
                 || !RTStrICmp(pArg->argv[1], "md"))
            rcExit = handleCtrlCreateDirectory(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "createtemporary")
                 || !RTStrICmp(pArg->argv[1], "createtemp")
                 || !RTStrICmp(pArg->argv[1], "mktemp"))
            rcExit = handleCtrlCreateTemp(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "kill")    /* Linux. */
                 || !RTStrICmp(pArg->argv[1], "pkill")   /* Solaris / *BSD. */
                 || !RTStrICmp(pArg->argv[1], "pskill")) /* SysInternals version. */
        {
            /** @todo What about "taskkill" on Windows? */
            rcExit = handleCtrlProcessClose(guest, &arg);
        }
        /** @todo Implement "killall"? */
        else if (   !RTStrICmp(pArg->argv[1], "stat"))
            rcExit = handleCtrlStat(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "updateadditions")
                 || !RTStrICmp(pArg->argv[1], "updateadds"))
            rcExit = handleCtrlUpdateAdditions(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "list"))
            rcExit = handleCtrlList(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "session"))
            rcExit = handleCtrlSession(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "process"))
            rcExit = handleCtrlProcess(guest, &arg);
        else if (   !RTStrICmp(pArg->argv[1], "watch"))
            rcExit = handleCtrlWatch(guest, &arg);
        else
            rcExit = errorSyntax(USAGE_GUESTCONTROL, "Unknown sub command '%s' specified!", pArg->argv[1]);

        ctrlUninitVM(pArg);
        return rcExit;
    }
    return RTEXITCODE_FAILURE;
}

#endif /* !VBOX_ONLY_DOCS */

