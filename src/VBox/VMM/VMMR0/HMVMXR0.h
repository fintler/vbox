/* $Id$ */
/** @file
 * HM VMX (VT-x) - Internal header file.
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

#ifndef ___HMVMXR0_h
#define ___HMVMXR0_h

RT_C_DECLS_BEGIN

/** @defgroup grp_vmx_int   Internal
 * @ingroup grp_vmx
 * @internal
 * @{
 */

#ifdef IN_RING0

VMMR0DECL(int)  VMXR0Enter(PVM pVM, PVMCPU pVCpu, PHMGLOBLCPUINFO pCpu);
VMMR0DECL(int)  VMXR0Leave(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR0DECL(int)  VMXR0EnableCpu(PHMGLOBLCPUINFO pCpu, PVM pVM, void *pvPageCpu, RTHCPHYS pPageCpuPhys, bool fEnabledBySystem);
VMMR0DECL(int)  VMXR0DisableCpu(PHMGLOBLCPUINFO pCpu, void *pvPageCpu, RTHCPHYS pPageCpuPhys);
VMMR0DECL(int)  VMXR0GlobalInit(void);
VMMR0DECL(void) VMXR0GlobalTerm(void);
VMMR0DECL(int)  VMXR0InitVM(PVM pVM);
VMMR0DECL(int)  VMXR0TermVM(PVM pVM);
VMMR0DECL(int)  VMXR0SetupVM(PVM pVM);
VMMR0DECL(int)  VMXR0SaveHostState(PVM pVM, PVMCPU pVCpu);
VMMR0DECL(int)  VMXR0LoadGuestState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR0DECL(int)  VMXR0RunGuestCode(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
DECLASM(int)    VMXR0StartVM32(RTHCUINT fResume, PCPUMCTX pCtx, PVMCSCACHE pCache, PVM pVM, PVMCPU pVCpu);
DECLASM(int)    VMXR0StartVM64(RTHCUINT fResume, PCPUMCTX pCtx, PVMCSCACHE pCache, PVM pVM, PVMCPU pVCpu);

# if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS) && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
DECLASM(int)    VMXR0SwitcherStartVM64(RTHCUINT fResume, PCPUMCTX pCtx, PVMCSCACHE pCache, PVM pVM, PVMCPU pVCpu);
VMMR0DECL(int)  VMXR0Execute64BitsHandler(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx, HM64ON32OP enmOp, uint32_t cbParam,
                                         uint32_t *paParam);
# endif

/* Cached VMCS accesses -- defined always in the old VT-x code, defined only for 32 hosts on new code. */
#ifdef VMX_USE_CACHED_VMCS_ACCESSES
VMMR0DECL(int) VMXWriteCachedVmcsEx(PVMCPU pVCpu, uint32_t idxField, uint64_t u64Val);

DECLINLINE(int) VMXReadCachedVmcsEx(PVMCPU pVCpu, uint32_t idxCache, RTGCUINTREG *pVal)
{
    Assert(idxCache <= VMX_VMCS_MAX_NESTED_PAGING_CACHE_IDX);
    *pVal = pVCpu->hm.s.vmx.VMCSCache.Read.aFieldVal[idxCache];
    return VINF_SUCCESS;
}
#endif

# ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
#  define VMXReadVmcsHstN(idxField, p64Val)               HMVMX_IS_64BIT_HOST_MODE() ?                      \
                                                            VMXReadVmcs64(idxField, p64Val)                 \
                                                          : (*(p64Val) &= UINT64_C(0xffffffff),             \
                                                             VMXReadVmcs32(idxField, (uint32_t *)(p64Val)))
/* Don't use fAllow64BitGuests for VMXReadVmcsGstN() even though it looks right, as it can be forced to 'true'.
   HMVMX_IS_64BIT_HOST_MODE() is what we need. */
#  define VMXReadVmcsGstN                                 VMXReadVmcsHstN
#  define VMXReadVmcsGstNByIdxVal                         VMXReadVmcsGstN
# elif HC_ARCH_BITS == 32
#  define VMXReadVmcsHstN                                 VMXReadVmcs32
#  define VMXReadVmcsGstN(idxField, pVal)                 VMXReadCachedVmcsEx(pVCpu, idxField##_CACHE_IDX, pVal)
#  define VMXReadVmcsGstNByIdxVal(idxField, pVal)         VMXReadCachedVmcsEx(pVCpu, idxField, pVal)
# else /* HC_ARCH_BITS == 64 */
#  define VMXReadVmcsHstN                                 VMXReadVmcs64
#  define VMXReadVmcsGstN                                 VMXReadVmcs64
#  define VMXReadVmcsGstNByIdxVal                         VMXReadVmcs64
# endif

#endif /* IN_RING0 */

/** @} */

RT_C_DECLS_END

#endif /* ___HMVMXR0_h */

