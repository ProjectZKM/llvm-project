//===- MipsOptimizePICCall.cpp - Optimize PIC Calls -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass eliminates unnecessary instructions that set up $gp and replace
// instructions that load target function addresses with copy instructions.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MipsBaseInfo.h"
#include "Mips.h"
#include "MipsRegisterInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/CodeGenTypes/MachineValueType.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/RecyclingAllocator.h"
#include <cassert>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "optimize-mips-loop-reduce-hilo"

namespace {
using Iter = MachineBasicBlock::iterator;
using ReverseIter = MachineBasicBlock::reverse_iterator;
using MBBandIwrite = std::pair<MachineBasicBlock *, MachineInstr *>;
class LoopReduceHiLo : public MachineFunctionPass {
public:
  LoopReduceHiLo() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips Loop Reduce HiLo"; }

  bool runOnMachineFunction(MachineFunction &F) override;

  static char ID;

private:
  /// Visit MBB.
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB);
  bool notReadAndWriteSrcAndHiLo(MachineBasicBlock &MBB, Iter I, Register reg);
  const MipsSubtarget *STI;
  const MipsInstrInfo *TII;
  MachineRegisterInfo *MRI;
  const TargetRegisterInfo *TRI;
};

} // end of anonymous namespace

char LoopReduceHiLo::ID = 0;

bool LoopReduceHiLo::runOnMachineFunction(MachineFunction &F) {
  STI = &F.getSubtarget<MipsSubtarget>();
  TII = STI->getInstrInfo();
  MRI = &F.getRegInfo();
  TRI = STI->getRegisterInfo();
  if (STI->inMips16Mode() || !STI->hasMips32r2() || STI->hasMips32r6())
    return false;

  bool Changed = false;
  for (MachineBasicBlock &MBB : F)
    Changed |= runOnMachineBasicBlock(MBB);

  return Changed;
}

// The source register and HiLo is not read and write before this instruction in
// this MBB
bool LoopReduceHiLo::notReadAndWriteSrcAndHiLo(MachineBasicBlock &MBB, Iter I,
                                               Register reg) {
  ReverseIter rI(std::prev(I));
  for (; rI != MBB.rend(); rI++) {
    unsigned Opcode = rI->getOpcode();
    MachineOperand Op0 = rI->getOperand(0);
    if (IsMFLOMFHI(Opcode) || IsPseudoMFLOMFHI(Opcode) || IsMTLOMTHI(Opcode) ||
        IsPseudoMTLOHI(Opcode) || IsDIVMULT(Opcode) || IsMAddMSub(Opcode))
      return false;
    if (rI->readsRegister(reg, TRI))
      return false;
    if (Op0.isReg() && Op0.getReg() == reg)
      return false;
  }
  return true;
}

static SmallVector<MBBandIwrite, 4>
allPredecessorsSetReg(MachineBasicBlock &MBB, MachineInstr *I, Register reg) {
  bool SeeSelf = false;
  SmallVector<MBBandIwrite, 4> Result;
  for (auto *P : MBB.predecessors()) {
    if (P == &MBB) {
      SeeSelf = true;
    }
    if (P != &MBB && P->succ_size() != 1) {
      Result.clear();
      return Result;
    }
    ReverseIter rI;
    for (rI = P->rbegin(); rI != P->rend(); ++rI) {
      MachineOperand Op0 = rI->getOperand(0);
      unsigned Opcode = rI->getOpcode();
      if (P == &MBB && I == rI) {
        Result.clear();
        return Result;
      }
      if (!Op0.isReg() || Op0.getReg() != reg)
        continue;
      if (P == &MBB && !IsPseudoMFLOMFHI(Opcode)) {
        Result.clear();
        return Result;
      }
      break;
    }
    if (rI == P->rend()) {
      Result.clear();
      return Result;
    }
    Result.push_back({P, &*rI});
  }
  if (!SeeSelf) {
    Result.clear();
    return Result;
  }

  return Result;
}

bool LoopReduceHiLo::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Changed = false;
  SmallVector<MBBandIwrite, 4> MBBWriteInfo;
  Register SrcLo;
  Register HiLoReg;
  bool Is64 = false;
  MachineInstr *InstMTHiLo;
  for (Iter I = MBB.begin(); I != MBB.end(); ++I) {
    unsigned IOpcode = I->getOpcode();
    if (!IsPseudoMTLOHI(IOpcode))
      continue;
    if (IOpcode == Mips::PseudoMTLOHI64)
      Is64 = true;
    InstMTHiLo = &*I;
    HiLoReg = I->getOperand(0).getReg();
    SrcLo = I->getOperand(1).getReg();
    if (!I->getOperand(2).isUndef())
      return Changed;
    if (!notReadAndWriteSrcAndHiLo(MBB, I, SrcLo))
      return Changed;
    MBBWriteInfo.append(allPredecessorsSetReg(MBB, &*I, SrcLo));
    if (MBBWriteInfo.empty())
      return Changed;
    break;
  }
  if (MBBWriteInfo.empty())
    return Changed;
  if (MBB.succ_size() != 2)
    return Changed;
  MachineBasicBlock *SMBB;
  for (const auto Succ : MBB.successors()) {
    if (Succ != &MBB) {
      SMBB = Succ;
      break;
    }
  }
  if (SMBB->pred_size() != 1)
    return Changed;
  Iter SMBBMFLoInstr;
  for (Iter I = SMBB->begin(); I != SMBB->end(); I++) {
    SMBBMFLoInstr = I;
    if (I->readsRegister(SrcLo, TRI)) {
      break;
    }
    if (I->getOperand(0).getReg() == SrcLo) {
      I = SMBB->end();
      break;
    }
  }
  if (SMBBMFLoInstr == SMBB->end())
    return Changed;

  Changed = true;
  if (!SMBB->isLiveIn(HiLoReg))
    SMBB->addLiveIn(HiLoReg);
  BuildMI(*SMBB, SMBBMFLoInstr, SMBBMFLoInstr->getDebugLoc(),
          TII->get(Is64 ? Mips::PseudoMFLO64 : Mips::PseudoMFLO), SrcLo)
      .addReg(HiLoReg);
  if (!MBB.isLiveIn(HiLoReg))
    MBB.addLiveIn(HiLoReg);
  InstMTHiLo->eraseFromParent();
  MBB.removeLiveIn(SrcLo);
  for (auto MBWI : MBBWriteInfo) {
    auto PMBB = MBWI.first;
    auto I = MBWI.second;
    if (PMBB != &MBB) {
      DebugLoc dl = I->getDebugLoc();
      unsigned MTLoOpcode = Is64 ? Mips::PseudoMTLOHI64 : Mips::PseudoMTLOHI;
      Iter It = I->getIterator();
      ++It;
      BuildMI(*PMBB, It, dl, TII->get(MTLoOpcode), HiLoReg)
          .addReg(SrcLo)
          .addReg(SrcLo, RegState::Undef);
    } else {
      I->eraseFromParent();
    }
  }
  return Changed;
}

/// Return an CombineMulAdd object.
FunctionPass *llvm::createMipsLoopReduceHiLoPass() {
  return new LoopReduceHiLo();
}
