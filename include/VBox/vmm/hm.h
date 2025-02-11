/** @file
 * HM - Intel/AMD VM Hardware Assisted Virtualization Manager (VMM)
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

#ifndef ___VBox_vmm_hm_h
#define ___VBox_vmm_hm_h

#include <VBox/vmm/pgm.h>
#include <VBox/vmm/cpum.h>
#include <VBox/vmm/vmm.h>
#include <iprt/mp.h>


/** @defgroup grp_hm      The VM Hardware Manager API
 * @{
 */

RT_C_DECLS_BEGIN

/**
 * Checks whether HM (VT-x/AMD-V) is being used by this VM.
 *
 * @retval  @c true if used.
 * @retval  @c false if software virtualization (raw-mode) is used.
 *
 * @param   a_pVM       The cross context VM structure.
 * @sa      HMIsEnabledNotMacro, HMR3IsEnabled
 * @internal
 */
#if defined(VBOX_STRICT) && defined(IN_RING3)
# define HMIsEnabled(a_pVM)   HMIsEnabledNotMacro(a_pVM)
#else
# define HMIsEnabled(a_pVM)   ((a_pVM)->fHMEnabled)
#endif

/**
 * Checks whether raw-mode context is required for any purpose.
 *
 * @retval  @c true if required either by raw-mode itself or by HM for doing
 *          switching the cpu to 64-bit mode.
 * @retval  @c false if not required.
 *
 * @param   a_pVM       The cross context VM structure.
 * @internal
 */
#if HC_ARCH_BITS == 64
# define HMIsRawModeCtxNeeded(a_pVM)   (!HMIsEnabled(a_pVM))
#else
# define HMIsRawModeCtxNeeded(a_pVM)   (!HMIsEnabled(a_pVM) || (a_pVM)->fHMNeedRawModeCtx)
#endif

 /**
 * Check if the current CPU state is valid for emulating IO blocks in the recompiler
 *
 * @returns boolean
 * @param   a_pVCpu     Pointer to the shared virtual CPU structure.
 * @internal
 */
#define HMCanEmulateIoBlock(a_pVCpu)     (!CPUMIsGuestInPagedProtectedMode(a_pVCpu))

 /**
 * Check if the current CPU state is valid for emulating IO blocks in the recompiler
 *
 * @returns boolean
 * @param   a_pCtx      Pointer to the CPU context (within PVM).
 * @internal
 */
#define HMCanEmulateIoBlockEx(a_pCtx)   (!CPUMIsGuestInPagedProtectedModeEx(a_pCtx))

/**
 * Checks whether we're in the special hardware virtualization context.
 * @returns true / false.
 * @param   a_pVCpu     The caller's cross context virtual CPU structure.
 * @thread  EMT
 */
#ifdef IN_RING0
# define HMIsInHwVirtCtx(a_pVCpu)       (VMCPU_GET_STATE(a_pVCpu) == VMCPUSTATE_STARTED_HM)
#else
# define HMIsInHwVirtCtx(a_pVCpu)       (false)
#endif

/**
 * Checks whether we're in the special hardware virtualization context and we
 * cannot perform long jump without guru meditating and possibly messing up the
 * host and/or guest state.
 *
 * This is after we've turned interrupts off and such.
 *
 * @returns true / false.
 * @param   a_pVCpu     The caller's cross context virtual CPU structure.
 * @thread  EMT
 */
#ifdef IN_RING0
# define HMIsInHwVirtNoLongJmpCtx(a_pVCpu)  (VMCPU_GET_STATE(a_pVCpu) == VMCPUSTATE_STARTED_EXEC)
#else
# define HMIsInHwVirtNoLongJmpCtx(a_pVCpu)  (false)
#endif

/**
 * 64-bit raw-mode (intermediate memory context) operations.
 *
 * These are special hypervisor eip values used when running 64-bit guests on
 * 32-bit hosts. Each operation corresponds to a routine.
 *
 * @note Duplicated in the assembly code!
 */
typedef enum HM64ON32OP
{
    HM64ON32OP_INVALID = 0,
    HM64ON32OP_VMXRCStartVM64,
    HM64ON32OP_SVMRCVMRun64,
    HM64ON32OP_HMRCSaveGuestFPU64,
    HM64ON32OP_HMRCSaveGuestDebug64,
    HM64ON32OP_HMRCTestSwitcher64,
    HM64ON32OP_END,
    HM64ON32OP_32BIT_HACK = 0x7fffffff
} HM64ON32OP;

VMMDECL(bool)                   HMIsEnabledNotMacro(PVM pVM);
VMM_INT_DECL(int)               HMInvalidatePage(PVMCPU pVCpu, RTGCPTR GCVirt);
VMM_INT_DECL(bool)              HMHasPendingIrq(PVM pVM);
VMM_INT_DECL(PX86PDPE)          HMGetPaePdpes(PVMCPU pVCpu);
VMM_INT_DECL(int)               HMAmdIsSubjectToErratum170(uint32_t *pu32Family, uint32_t *pu32Model, uint32_t *pu32Stepping);
VMM_INT_DECL(bool)              HMSetSingleInstruction(PVMCPU pVCpu, bool fEnable);

#ifndef IN_RC
VMM_INT_DECL(int)               HMFlushTLB(PVMCPU pVCpu);
VMM_INT_DECL(int)               HMFlushTLBOnAllVCpus(PVM pVM);
VMM_INT_DECL(int)               HMInvalidatePageOnAllVCpus(PVM pVM, RTGCPTR GCVirt);
VMM_INT_DECL(int)               HMInvalidatePhysPage(PVM pVM, RTGCPHYS GCPhys);
VMM_INT_DECL(bool)              HMIsNestedPagingActive(PVM pVM);
VMM_INT_DECL(PGMMODE)           HMGetShwPagingMode(PVM pVM);
#else /* Nops in RC: */
# define HMFlushTLB(pVCpu)                  do { } while (0)
# define HMIsNestedPagingActive(pVM)        false
# define HMFlushTLBOnAllVCpus(pVM)          do { } while (0)
#endif

#ifdef IN_RING0
/** @defgroup grp_hm_r0    The VM Hardware Manager API
 * @ingroup grp_hm
 * @{
 */
VMMR0_INT_DECL(int)             HMR0Init(void);
VMMR0_INT_DECL(int)             HMR0Term(void);
VMMR0_INT_DECL(int)             HMR0InitVM(PVM pVM);
VMMR0_INT_DECL(int)             HMR0TermVM(PVM pVM);
VMMR0_INT_DECL(int)             HMR0EnableAllCpus(PVM pVM);
VMMR0_INT_DECL(int)             HMR0EnterSwitcher(PVM pVM, VMMSWITCHER enmSwitcher, bool *pfVTxDisabled);
VMMR0_INT_DECL(void)            HMR0LeaveSwitcher(PVM pVM, bool fVTxDisabled);

VMMR0_INT_DECL(void)            HMR0SavePendingIOPortWrite(PVMCPU pVCpu, RTGCPTR GCPtrRip, RTGCPTR GCPtrRipNext,
                                                           unsigned uPort, unsigned uAndVal, unsigned cbSize);
VMMR0_INT_DECL(void)            HMR0SavePendingIOPortRead(PVMCPU pVCpu, RTGCPTR GCPtrRip, RTGCPTR GCPtrRipNext,
                                                          unsigned uPort, unsigned uAndVal, unsigned cbSize);

/** @} */
#endif /* IN_RING0 */


#ifdef IN_RING3
/** @defgroup grp_hm_r3    The VM Hardware Manager API
 * @ingroup grp_hm
 * @{
 */
VMMR3DECL(bool)                 HMR3IsEnabled(PUVM pUVM);
VMMR3DECL(bool)                 HMR3IsNestedPagingActive(PUVM pUVM);
VMMR3DECL(bool)                 HMR3IsVpidActive(PUVM pVUM);
VMMR3DECL(bool)                 HMR3IsUXActive(PUVM pVUM);

VMMR3_INT_DECL(bool)            HMR3IsEventPending(PVMCPU pVCpu);
VMMR3_INT_DECL(int)             HMR3Init(PVM pVM);
VMMR3_INT_DECL(int)             HMR3InitCompleted(PVM pVM, VMINITCOMPLETED enmWhat);
VMMR3_INT_DECL(void)            HMR3Relocate(PVM pVM);
VMMR3_INT_DECL(int)             HMR3Term(PVM pVM);
VMMR3_INT_DECL(void)            HMR3Reset(PVM pVM);
VMMR3_INT_DECL(void)            HMR3ResetCpu(PVMCPU pVCpu);
VMMR3_INT_DECL(void)            HMR3CheckError(PVM pVM, int iStatusCode);
VMMR3DECL(bool)                 HMR3CanExecuteGuest(PVM pVM, PCPUMCTX pCtx);
VMMR3_INT_DECL(void)            HMR3NotifyScheduled(PVMCPU pVCpu);
VMMR3_INT_DECL(void)            HMR3NotifyEmulated(PVMCPU pVCpu);
VMMR3_INT_DECL(bool)            HMR3IsActive(PVMCPU pVCpu);
VMMR3_INT_DECL(void)            HMR3PagingModeChanged(PVM pVM, PVMCPU pVCpu, PGMMODE enmShadowMode, PGMMODE enmGuestMode);
VMMR3_INT_DECL(int)             HMR3EmulateIoBlock(PVM pVM, PCPUMCTX pCtx);
VMMR3_INT_DECL(VBOXSTRICTRC)    HMR3RestartPendingIOInstr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR3_INT_DECL(int)             HMR3EnablePatching(PVM pVM, RTGCPTR pPatchMem, unsigned cbPatchMem);
VMMR3_INT_DECL(int)             HMR3DisablePatching(PVM pVM, RTGCPTR pPatchMem, unsigned cbPatchMem);
VMMR3_INT_DECL(int)             HMR3PatchTprInstr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR3_INT_DECL(bool)            HMR3IsRescheduleRequired(PVM pVM, PCPUMCTX pCtx);
VMMR3_INT_DECL(bool)            HMR3IsVmxPreemptionTimerUsed(PVM pVM);

/** @} */
#endif /* IN_RING3 */

#ifdef IN_RING0
/** @addtogroup grp_hm_r0
 * @{
 */
VMMR0_INT_DECL(int)             HMR0SetupVM(PVM pVM);
VMMR0_INT_DECL(int)             HMR0RunGuestCode(PVM pVM, PVMCPU pVCpu);
VMMR0_INT_DECL(int)             HMR0Enter(PVM pVM, PVMCPU pVCpu);
VMMR0_INT_DECL(int)             HMR0Leave(PVM pVM, PVMCPU pVCpu);
VMMR0_INT_DECL(bool)            HMR0SuspendPending(void);

# if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS)
VMMR0_INT_DECL(int)             HMR0SaveFPUState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR0_INT_DECL(int)             HMR0SaveDebugState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx);
VMMR0_INT_DECL(int)             HMR0TestSwitcher3264(PVM pVM);
# endif

/** @} */
#endif /* IN_RING0 */


/** @} */
RT_C_DECLS_END


#endif

