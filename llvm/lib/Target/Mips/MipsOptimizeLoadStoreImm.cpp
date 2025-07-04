//===- MipsOptimizeLoadStore.cpp - Optimize Load/Store IMMs ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass looks for IMMs that can be filled into load/store IMM, instead of
// addiu etc.
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

#define DEBUG_TYPE "optimize-mips-load-store-imm"

namespace {

using Iter = MachineBasicBlock::iterator;
using ReverseIter = MachineBasicBlock::reverse_iterator;

class OptimizeLoadStoreImm : public MachineFunctionPass {
public:
  OptimizeLoadStoreImm() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Mips OptimizeLoadStoreImm"; }

  bool runOnMachineFunction(MachineFunction &F) override;

private:
  static char ID;
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB);
  bool runOnMachineInstr(MachineBasicBlock &MBB, Iter I);
};

} // end of anonymous namespace

char OptimizeLoadStoreImm::ID = 0;

// OptimizeLoadStoreImm methods.
bool OptimizeLoadStoreImm::runOnMachineFunction(MachineFunction &F) {
  if (F.getSubtarget<MipsSubtarget>().inMips16Mode())
    return false;

  bool Changed = false;
  for (MachineBasicBlock &MBB : F)
    Changed |= runOnMachineBasicBlock(MBB);

  return Changed;
}

bool OptimizeLoadStoreImm::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Changed = false;

  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  for (Iter I = MBB.begin(); I != MBB.end();) {
    if (runOnMachineInstr(MBB, I)) {
      Changed = true;
      Register Dest = I->getOperand(0).getReg();
      Register Src = I->getOperand(1).getReg();
      MRI.replaceRegWith(Dest, Src);
      I = MBB.erase(I);
    } else {
      ++I;
    }
  }
  return Changed;
}

static bool isInputLoZero(MachineBasicBlock &MBB, Iter I) {
  MachineOperand &SrcOp = I->getOperand(1);
  if (!SrcOp.isReg())
    return false;
  Register SrcReg = SrcOp.getReg();

  for (ReverseIter rI(I); rI != MBB.rend(); rI++) {
    MachineOperand &Op0 = rI->getOperand(0);
    if (!Op0.isReg())
      continue;
    Register Op0Reg = Op0.getReg();
    if (Op0Reg != SrcReg)
      continue;
    unsigned rIOpcode = rI->getOpcode();
    if (rIOpcode == Mips::LUi || rIOpcode == Mips::LUi64)
      return true;
    else if (rIOpcode == Mips::SLL || rIOpcode == Mips::DSLL) {
      unsigned ShiftImm = rI->getOperand(2).getImm();
      if (ShiftImm >= 16)
        return true;
    } else
      return false;
  }
  return false;
}

static bool isLoadStoreWithImm16(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case Mips::LB:
  case Mips::LB64:
  case Mips::LBu:
  case Mips::LBu64:
  case Mips::LBuE:
  case Mips::LH:
  case Mips::LH64:
  case Mips::LHu:
  case Mips::LHu64:
  case Mips::LHuE:
  case Mips::LW:
  case Mips::LW64:
  case Mips::LWE:
  case Mips::LWu:
  case Mips::LWC1:
  case Mips::LWC2:
  case Mips::LWC3:
  case Mips::LD:
  case Mips::LDC1:
  case Mips::LDC164:
  case Mips::LDC2:
  case Mips::SB:
  case Mips::SB64:
  case Mips::SH:
  case Mips::SH64:
  case Mips::SW:
  case Mips::SW64:
  case Mips::SWE:
  case Mips::SWC1:
  case Mips::SWC2:
  case Mips::SWC3:
  case Mips::SD:
  case Mips::SDC1:
  case Mips::SDC164:
  case Mips::SDC2:
    return true;
  }
  return false;
}

bool OptimizeLoadStoreImm::runOnMachineInstr(MachineBasicBlock &MBB,
                                             Iter InstrWithImm) {
  bool Changed = false;

  unsigned IOpcode = InstrWithImm->getOpcode();
  switch (IOpcode) {
  default:
    return false;
  case Mips::ORi:
  case Mips::ORi64:
    if (!isInputLoZero(MBB, InstrWithImm))
      return false;
    break;
  case Mips::ADDiu:
  case Mips::DADDiu:
    break;
  }

  if (!InstrWithImm->getOperand(2).isImm())
    return false;
  int64_t Imm = InstrWithImm->getOperand(2).getImm();
  Register SrcReg = InstrWithImm->getOperand(0).getReg();

  for (Iter I = std::next(InstrWithImm); I != MBB.end(); ++I) {
    unsigned IOpcode = I->getOpcode();
    MachineOperand &Op0 = I->getOperand(0);
    if (Op0.isReg() && Op0.getReg() == SrcReg)
      return Changed;
    if (!I->readsVirtualRegister(SrcReg))
      continue;
    if (!Op0.isReg())
      return Changed;
    Register Reg0 = Op0.getReg();

    if (I->getNumOperands() != 2 && I->getNumOperands() != 3)
      return Changed;

    MachineOperand &Op1 = I->getOperand(1);
    if (I->getNumOperands() == 2) {
      if (!Op1.isKill())
        return Changed;
      if (IOpcode != Mips::COPY)
        return Changed;
      Register Reg1 = Op1.getReg();
      if (Reg1 != SrcReg)
        llvm_unreachable("Reg should be the same with SrcReg");
      SrcReg = Reg0;
      continue;
    }
    MachineOperand &Op2 = I->getOperand(2);
    if (IOpcode == Mips::ADDu || IOpcode == Mips::DADDu ||
        IOpcode == Mips::SUBu || IOpcode == Mips::DSUBu) {
      Register Reg1 = Op1.getReg();
      Register Reg2 = Op2.getReg();
      if (Reg1 == Reg2)
        return Changed;
      if (Reg1 == SrcReg) {
        if (!Op1.isKill())
          return Changed;
      } else if (Reg2 == SrcReg) {
        if (!Op2.isKill())
          return Changed;
        if (IOpcode == Mips::SUBu || IOpcode == Mips::DSUBu)
          Imm = -Imm;
      } else
        llvm_unreachable("Either of Operand1 or Operand2 should read SrcReg");
      SrcReg = Reg0;
      continue;
    } else if (IOpcode == Mips::ADDiu || IOpcode == Mips::DADDiu ||
               isLoadStoreWithImm16(IOpcode)) {
      Register Reg1 = Op1.getReg();
      if (!Op2.isImm())
        return Changed;
      uint64_t Imm2 = Op2.getImm();
      if (Reg1 != SrcReg)
        llvm_unreachable("Operand1 should read SrcReg");
      if (!Op1.isKill())
        return Changed;
      Imm += Imm2;
      if (Imm > 32767 || Imm < -32768)
        return false;
      I->getOperand(2).setImm(Imm);
      InstrWithImm->getOperand(2).setImm(0);
      Changed = true;
      return Changed;
    } else
      return Changed;
  }
  return Changed;
}

/// Return an OptimizeLoadStoreImm object.
FunctionPass *llvm::createMipsOptimizeLoadStoreImmPass() {
  return new OptimizeLoadStoreImm();
}
