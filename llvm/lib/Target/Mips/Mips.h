//===-- Mips.h - Top-level interface for Mips representation ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in
// the LLVM Mips back-end.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPS_H
#define LLVM_LIB_TARGET_MIPS_MIPS_H

#include "MCTargetDesc/MipsMCTargetDesc.h"
#include "llvm/Target/TargetMachine.h"

#define IsMFLOMFHI(instr)                                                      \
  (instr == Mips::MFLO || instr == Mips::MFLO64 || instr == Mips::MFHI ||      \
   instr == Mips::MFHI64)
#define IsPseudoMFLOMFHI(instr)                                                \
  (instr == Mips::PseudoMFLO || instr == Mips::PseudoMFLO64 ||                 \
   instr == Mips::PseudoMFHI || instr == Mips::PseudoMFHI64)
#define IsMTLOMTHI(instr)                                                      \
  (instr == Mips::MTLO || instr == Mips::MTLO64 || instr == Mips::MTHI ||      \
   instr == Mips::MTHI64)
#define IsPseudoMTLOHI(instr)                                                  \
  (instr == Mips::PseudoMTLOHI || instr == Mips::PseudoMTLOHI64)
#define IsDIVMULT(instr)                                                       \
  (instr == Mips::SDIV || instr == Mips::PseudoSDIV || instr == Mips::DSDIV || \
   instr == Mips::PseudoDSDIV || instr == Mips::UDIV ||                        \
   instr == Mips::PseudoUDIV || instr == Mips::DUDIV ||                        \
   instr == Mips::PseudoDUDIV || instr == Mips::MULT ||                        \
   instr == Mips::PseudoMULT || instr == Mips::DMULT ||                        \
   instr == Mips::PseudoDMULT)
#define IsMAddMSub(instr)                                                      \
  (instr == Mips::MADD || instr == Mips::MADDU || instr == Mips::MSUB ||       \
   instr == Mips::MSUBU || instr == Mips::PseudoMADD ||                        \
   instr == Mips::PseudoMADDU || instr == Mips::PseudoMSUB ||                  \
   instr == Mips::PseudoMSUBU)

namespace llvm {
class FunctionPass;
class InstructionSelector;
class MipsRegisterBankInfo;
class MipsSubtarget;
class MipsTargetMachine;
class MipsTargetMachine;
class ModulePass;
class PassRegistry;

ModulePass *createMipsOs16Pass();
ModulePass *createMips16HardFloatPass();

FunctionPass *createMipsModuleISelDagPass();
FunctionPass *createMipsOptimizePICCallPass();
FunctionPass *createMipsDelaySlotFillerPass();
FunctionPass *createMipsBranchExpansion();
FunctionPass *createMipsConstantIslandPass();
FunctionPass *createMicroMipsSizeReducePass();
FunctionPass *createMipsExpandPseudoPass();
FunctionPass *createMipsPreLegalizeCombiner();
FunctionPass *createMipsPostLegalizeCombiner(bool IsOptNone);
FunctionPass *createMipsMulMulBugPass();
FunctionPass *createMipsLoopReduceHiLoPass();

InstructionSelector *
createMipsInstructionSelector(const MipsTargetMachine &, const MipsSubtarget &,
                              const MipsRegisterBankInfo &);

void initializeMicroMipsSizeReducePass(PassRegistry &);
void initializeMipsAsmPrinterPass(PassRegistry &);
void initializeMipsBranchExpansionPass(PassRegistry &);
void initializeMipsDAGToDAGISelLegacyPass(PassRegistry &);
void initializeMipsDelaySlotFillerPass(PassRegistry &);
void initializeMipsMulMulBugFixPass(PassRegistry &);
void initializeMipsPostLegalizerCombinerPass(PassRegistry &);
void initializeMipsPreLegalizerCombinerPass(PassRegistry &);
} // namespace llvm

#endif
