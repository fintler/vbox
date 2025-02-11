/* $Id$ */
/** @file
 * VBoxServiceVMInfo.h - Internal VM info definitions.
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___VBoxServiceVMInfo_h
#define ___VBoxServiceVMInfo_h

//RT_C_DECLS_BEGIN

extern int vboxServiceUserUpdateF(PVBOXSERVICEVEPROPCACHE pCache, const char *pszUser, const char *pszDomain,
                                  const char *pszKey, const char *pszValueFormat, ...);

//RT_C_DECLS_END

extern uint32_t g_uVMInfoUserIdleThreshold;

#endif /* ___VBoxServiceVMInfo_h */

