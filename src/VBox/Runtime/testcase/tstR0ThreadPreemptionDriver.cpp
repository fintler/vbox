/* $Id$ */
/** @file
 * IPRT R0 Testcase - Thread Preemption, driver program.
 */

/*
 * Copyright (C) 2009-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/initterm.h>

#include <iprt/asm.h>
#include <iprt/cpuset.h>
#include <iprt/err.h>
#include <iprt/path.h>
#include <iprt/param.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/test.h>
#include <iprt/time.h>
#include <iprt/thread.h>
#ifdef VBOX
# include <VBox/sup.h>
# include "tstR0ThreadPreemption.h"
#endif

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
static bool volatile g_fTerminate = false;


/**
 * Try make sure all online CPUs will be engaged.
 */
static DECLCALLBACK(int) MyThreadProc(RTTHREAD hSelf, void *pvCpuIdx)
{
    RTCPUSET Affinity;
    RTCpuSetEmpty(&Affinity);
    RTCpuSetAddByIndex(&Affinity, (intptr_t)pvCpuIdx);
    RTThreadSetAffinity(&Affinity); /* ignore return code as it's not supported on all hosts. */

    while (!g_fTerminate)
    {
        uint64_t tsStart = RTTimeMilliTS();
        do
        {
            ASMNopPause();
        } while (RTTimeMilliTS() - tsStart < 8);
        RTThreadSleep(4);
    }

    return VINF_SUCCESS;
}


int main(int argc, char **argv)
{
#ifndef VBOX
    RTPrintf("tstSup: SKIPPED\n");
    return 0;
#else
    /*
     * Init.
     */
    RTTEST hTest;
    int rc = RTTestInitAndCreate("tstR0ThreadPreemption", &hTest);
    if (rc)
        return rc;
    RTTestBanner(hTest);

    PSUPDRVSESSION pSession;
    rc = SUPR3Init(&pSession);
    if (RT_FAILURE(rc))
    {
        RTTestFailed(hTest, "SUPR3Init failed with rc=%Rrc\n", rc);
        return RTTestSummaryAndDestroy(hTest);
    }

    char szPath[RTPATH_MAX];
    rc = RTPathExecDir(szPath, sizeof(szPath));
    if (RT_SUCCESS(rc))
        rc = RTPathAppend(szPath, sizeof(szPath), "tstR0ThreadPreemption.r0");
    if (RT_FAILURE(rc))
    {
        RTTestFailed(hTest, "Failed constructing .r0 filename (rc=%Rrc)", rc);
        return RTTestSummaryAndDestroy(hTest);
    }

    void *pvImageBase;
    rc = SUPR3LoadServiceModule(szPath, "tstR0ThreadPreemption",
                                "TSTR0ThreadPreemptionSrvReqHandler",
                                &pvImageBase);
    if (RT_FAILURE(rc))
    {
        RTTestFailed(hTest, "SUPR3LoadServiceModule(%s,,,) failed with rc=%Rrc\n", szPath, rc);
        return RTTestSummaryAndDestroy(hTest);
    }

    /* test request */
    struct
    {
        SUPR0SERVICEREQHDR  Hdr;
        char                szMsg[256];
    } Req;

    /*
     * Sanity checks.
     */
    RTTestSub(hTest, "Sanity");
    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREMEPTION_SANITY_OK, 0, &Req.Hdr), VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return RTTestSummaryAndDestroy(hTest);
    RTTESTI_CHECK_MSG(Req.szMsg[0] == '\0', ("%s", Req.szMsg));
    if (Req.szMsg[0] != '\0')
        return RTTestSummaryAndDestroy(hTest);

    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREMEPTION_SANITY_FAILURE, 0, &Req.Hdr), VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return RTTestSummaryAndDestroy(hTest);
    RTTESTI_CHECK_MSG(!strncmp(Req.szMsg, RT_STR_TUPLE("!42failure42")), ("%s", Req.szMsg));
    if (strncmp(Req.szMsg, RT_STR_TUPLE("!42failure42")))
        return RTTestSummaryAndDestroy(hTest);

    /*
     * Basic tests, bail out on failure.
     */
    RTTestSub(hTest, "Basics");
    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREMEPTION_BASIC, 0, &Req.Hdr), VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return RTTestSummaryAndDestroy(hTest);
    if (Req.szMsg[0] == '!')
    {
        RTTestIFailed("%s", &Req.szMsg[1]);
        return RTTestSummaryAndDestroy(hTest);
    }
    if (Req.szMsg[0])
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%s", Req.szMsg);

    /*
     * Is it trusty.
     */
    RTTestSub(hTest, "RTThreadPreemptIsPendingTrusty");
    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREMEPTION_IS_TRUSTY, 0, &Req.Hdr), VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return RTTestSummaryAndDestroy(hTest);
    if (Req.szMsg[0] == '!')
        RTTestIFailed("%s", &Req.szMsg[1]);
    else if (Req.szMsg[0])
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%s", Req.szMsg);

    /*
     * Stay in ring-0 until preemption is pending.
     */
    RTTHREAD ahThreads[RTCPUSET_MAX_CPUS];
    uint32_t cThreads = RTMpGetCount();
    RTCPUSET OnlineSet;
    RTMpGetOnlineSet(&OnlineSet);
    for (uint32_t i = 0; i < RT_ELEMENTS(ahThreads); i++)
    {
        ahThreads[i] = NIL_RTTHREAD;
        if (RTCpuSetIsMemberByIndex(&OnlineSet, i))
            RTThreadCreateF(&ahThreads[i], MyThreadProc, (void *)(uintptr_t)i, 0, RTTHREADTYPE_DEFAULT,
                            RTTHREADFLAGS_WAITABLE, "cpu=%u", i);
    }


    RTTestSub(hTest, "Pending Preemption");
RTThreadSleep(250); /** @todo fix GIP initialization? */
    for (int i = 0; ; i++)
    {
        Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
        Req.Hdr.cbReq = sizeof(Req);
        Req.szMsg[0] = '\0';
        RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                                 TSTR0THREADPREMEPTION_IS_PENDING, 0, &Req.Hdr), VINF_SUCCESS);
        if (    strcmp(Req.szMsg, "!cLoops=1\n")
            ||  i >= 64)
        {
            if (Req.szMsg[0] == '!')
                RTTestIFailed("%s", &Req.szMsg[1]);
            else if (Req.szMsg[0])
                RTTestIPrintf(RTTESTLVL_ALWAYS, "%s", Req.szMsg);
            break;
        }
        if ((i % 3) == 0)
            RTThreadYield();
        else if ((i % 16) == 0)
            RTThreadSleep(8);
    }

    ASMAtomicWriteBool(&g_fTerminate, true);
    for (uint32_t i = 0; i < RT_ELEMENTS(ahThreads); i++)
        if (ahThreads[i] != NIL_RTTHREAD)
            RTThreadWait(ahThreads[i], 5000, NULL);

    /*
     * Test nested RTThreadPreemptDisable calls.
     */
    RTTestSub(hTest, "Nested");
    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREMEPTION_NESTED, 0, &Req.Hdr), VINF_SUCCESS);
    if (Req.szMsg[0] == '!')
        RTTestIFailed("%s", &Req.szMsg[1]);
    else if (Req.szMsg[0])
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%s", Req.szMsg);


    /*
     * Test thread-context hooks.
     */
    RTTestSub(hTest, "RTThreadCtxHooks");
    Req.Hdr.u32Magic = SUPR0SERVICEREQHDR_MAGIC;
    Req.Hdr.cbReq = sizeof(Req);
    Req.szMsg[0] = '\0';
    RTTESTI_CHECK_RC(rc = SUPR3CallR0Service("tstR0ThreadPreemption", sizeof("tstR0ThreadPreemption") - 1,
                                             TSTR0THREADPREEMPTION_CTXHOOKS, 0, &Req.Hdr), VINF_SUCCESS);
    if (RT_FAILURE(rc))
        return RTTestSummaryAndDestroy(hTest);
    if (Req.szMsg[0] == '!')
        RTTestIFailed("%s", &Req.szMsg[1]);
    else if (Req.szMsg[0])
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%s", Req.szMsg);

    /*
     * Done.
     */
    return RTTestSummaryAndDestroy(hTest);
#endif
}

