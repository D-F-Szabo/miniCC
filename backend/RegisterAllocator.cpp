#include "RegisterAllocator.hpp"
#include "MachineIRModule.hpp"
#include "MachineOperand.hpp"
#include "Support.hpp"
#include "TargetInstruction.hpp"
#include "TargetMachine.hpp"
#include "TargetRegister.hpp"
#include <algorithm>
#include <vector>

using VirtualReg = unsigned;
using PhysicalReg = unsigned;

void PreAllocateParameters(MachineFunction &Func, TargetMachine *TM,
                           std::map<VirtualReg, PhysicalReg> &AllocatedRegisters) {
  auto ArgRegs = TM->GetABI()->GetArgumentRegisters();
  unsigned CurrentParamReg = 0;

  for (auto Param : Func.GetParameters()) {
    // FIXME: excess parameters should be stored on the stack
    assert(CurrentParamReg < ArgRegs.size() && "Run out of param regs");
    // allocate the param to the CurrentParamReg -th param register
    if (Param.second.GetBitWidth() <= 32)
      AllocatedRegisters[Param.first] = ArgRegs[CurrentParamReg++]->GetSubRegs()[0];
    else
      AllocatedRegisters[Param.first] = ArgRegs[CurrentParamReg++]->GetID();
  }
}

void PreAllocateReturnRegister(
    MachineFunction &Func, TargetMachine *TM,
    std::map<VirtualReg, PhysicalReg> &AllocatedRegisters) {
  auto RetRegs = TM->GetABI()->GetReturnRegisters();
  auto LastBBInstrs = Func.GetBasicBlocks().back().GetInstructions();

  for (auto It = LastBBInstrs.rbegin(); It != LastBBInstrs.rend(); It++) {
    // If return instruction
    auto Opcode = It->GetOpcode();
    if (auto TargetInstr = TM->GetInstrDefs()->GetTargetInstr(Opcode);
        TargetInstr->IsReturn()) {
      auto RetValSize = It->GetOperands()[0].GetSize();

      if (RetValSize == RetRegs[0]->GetBitWidth())
        AllocatedRegisters[It->GetOperand(0)->GetReg()] = RetRegs[0]->GetID();
      else
        AllocatedRegisters[It->GetOperand(0)->GetReg()] = RetRegs[0]->GetSubRegs()[0];
    }
  }
}

PhysicalReg GetNextAvailableReg(uint8_t BitSize, std::vector<PhysicalReg> &Pool,
                                TargetMachine *TM) {
  unsigned loopCounter = 0;
  for (auto UnAllocatedReg : Pool) {
    auto UnAllocatedRegInfo = TM->GetRegInfo()->GetRegisterByID(UnAllocatedReg);
    // If the register bit width matches the requested size then return this
    // register and delete it from the pool
    if (UnAllocatedRegInfo->GetBitWidth() == BitSize) {
      Pool.erase(Pool.begin() + loopCounter);
      return UnAllocatedReg;
    }
    // Otherwise check the subregisters of the register if it has, and try to
    // find a right candidate
    for (auto SubReg : UnAllocatedRegInfo->GetSubRegs()) {
      auto SubRegInfo = TM->GetRegInfo()->GetRegisterByID(SubReg);
      if (SubRegInfo->GetBitWidth() == BitSize) {
        Pool.erase(Pool.begin() + loopCounter);
        return SubReg;
      }
    }

    loopCounter++;
  }

  assert(!"Have not found the right registers");
  return 0;
}

PhysicalReg GetMatchingSizedRegFromSubRegs(PhysicalReg PhysReg, uint8_t BitSize,
                                           TargetMachine *TM) {
  auto PhysRegInfo = TM->GetRegInfo()->GetRegisterByID(PhysReg);

  // if the register size is not matching the actual operand size
  // in bits then search for the subregisters as well for actual
  // matching one
  if (PhysRegInfo->GetBitWidth() == BitSize)
    return PhysReg;

  for (auto SubReg : PhysRegInfo->GetSubRegs()) {
    auto PhysSubRegInfo = TM->GetRegInfo()->GetRegisterByID(SubReg);

    if (PhysSubRegInfo->GetBitWidth() == BitSize)
      return SubReg;
  }

  return ~0u;
}

void RegisterAllocator::RunRA() {
  // mapping from virtual reg to physical
  std::map<VirtualReg, PhysicalReg> AllocatedRegisters;
  std::vector<PhysicalReg> RegisterPool;  // available registers
  std::set<MachineOperand> RegistersToSpill; // register require spilling

  for (auto &Func : MIRM->GetFunctions()) {
    AllocatedRegisters.clear();
    RegisterPool.clear();
    RegistersToSpill.clear();

    // Initialize the usable register's pool
    for (auto TargetReg : TM->GetABI()->GetCallerSavedRegisters())
      RegisterPool.push_back(TargetReg->GetID());

    PreAllocateParameters(Func, TM, AllocatedRegisters);
    PreAllocateReturnRegister(Func, TM, AllocatedRegisters);

    // Remove the pre allocated registers from the register pool
    for (const auto [VirtReg, PhysReg] : AllocatedRegisters) {
      auto RegsToCheck = TM->GetRegInfo()->GetRegisterByID(PhysReg)->GetSubRegs();
      RegsToCheck.push_back(PhysReg);
      auto ParentReg = TM->GetRegInfo()->GetParentReg(PhysReg);
      if (ParentReg)
        RegsToCheck.push_back(ParentReg->GetID());

      for (auto Reg : RegsToCheck) {
        auto position = std::find(RegisterPool.begin(), RegisterPool.end(),
                                  Reg);
        if (position != RegisterPool.end())
          RegisterPool.erase(position);
      }
    }

    // we want to keep track of how many consecutive renames happened since only
    // two can be afforded for now, doing more then that is an error
    unsigned ConsecutiveLoadRenames = 0;
    unsigned ConsecutiveStoreRenames = 0;

    for (auto &BB : Func.GetBasicBlocks())
      for (auto &Instr : BB.GetInstructions())
        for (auto &Operand : Instr.GetOperands())
          if (Operand.IsVirtualReg() || Operand.IsParameter()) {
            const auto UsedReg = Operand.GetReg();
            bool AlreadyAllocated = 0 != AllocatedRegisters.count(UsedReg);

            // when short on allocatable registers, then add the register
            // to the spillable set if its not a load or a store
            if (!AlreadyAllocated && RegisterPool.size() <= 3) {
              // if the instruction is NOT a load, then spill it
              if (!Instr.IsLoadOrStore()) {
                ConsecutiveLoadRenames = ConsecutiveStoreRenames = 0;
                RegistersToSpill.insert(Operand);
                continue;
              }

              // Depending on whether its a load or a store, rename the register
              // (allocating essentially)
              if (Instr.IsLoad()) {
                // Note that the last 2 register is used for this purpose
                assert(ConsecutiveLoadRenames < 2);
                auto PhysReg =
                    RegisterPool.rbegin()[1 - ConsecutiveLoadRenames];
                auto FoundPhysReg = GetMatchingSizedRegFromSubRegs(
                    PhysReg, Operand.GetSize(), TM);
                assert(FoundPhysReg != ~0u &&
                       "Cannot found matching sized register");
                AllocatedRegisters[UsedReg] = FoundPhysReg;
                ConsecutiveLoadRenames++;
              } else {
                auto PhysReg = RegisterPool.rbegin()[2];
                auto FoundPhysReg = GetMatchingSizedRegFromSubRegs(
                    PhysReg, Operand.GetSize(), TM);
                assert(FoundPhysReg != ~0u &&
                       "Cannot found matching sized register");
                AllocatedRegisters[UsedReg] = FoundPhysReg;
                ConsecutiveStoreRenames++;
              }

              // if its a load then rename it, which means to allocate it one of
              // the registers used for loading spilled regs
              assert(ConsecutiveLoadRenames <= 2 &&
                     "To much consecutive loads to rename");
              assert(ConsecutiveStoreRenames <= 1 &&
                     "To much consecutive stores to rename");

              AlreadyAllocated = true;
            }

            // if not allocated yet
            if (!AlreadyAllocated) {
              // then allocate it
              auto Reg =
                  GetNextAvailableReg(Operand.GetSize(), RegisterPool, TM);
              AllocatedRegisters[UsedReg] = Reg;
              Operand.SetToRegister();
              Operand.SetReg(Reg);
            }
            // else its already allocated so just look it up
            else {
              Operand.SetToRegister();
              Operand.SetReg(AllocatedRegisters[UsedReg]);
            }
          }

    ///////////// Handle Spills

    // if there is nothing to spill then nothing to do
    if (RegistersToSpill.size() != 0) {
      for (auto &Reg : RegistersToSpill)
        Func.InsertStackSlot(Reg.GetReg(), Reg.GetSize() / 8);

      for (auto &BB : Func.GetBasicBlocks()) {
        auto &Instructions = BB.GetInstructions();

        for (size_t i = 0; i < Instructions.size(); i++) {

          // Check operands which use the register
          // (the first operand defining the reg the rest uses regs)
          for (size_t OpIndex = 1;
               OpIndex < Instructions[i].GetOperands().size(); OpIndex++) {
            auto &Operand = Instructions[i].GetOperands()[OpIndex];

            // only check register operands
            if (!(Operand.IsRegister() || Operand.IsMemory()))
              continue;

            auto UsedReg = Operand.GetReg();

            // if the used register is not spilled, then nothing to do
            if (RegistersToSpill.count(Operand) == 0)
              continue;

            ////// Insert a LOAD

            auto FoundReg =
                GetMatchingSizedRegFromSubRegs(RegisterPool[OpIndex],
                                               Operand.GetSize(), TM);
            assert(FoundReg != ~0u && "Cannot found matching sized register");

            Operand.SetReg(FoundReg);

            // Saving the original Operand to be able to use its information
            // when creating the LOAD
            auto OperandSave = Operand;

            // Make the operand into a physical register which using
            // the register used for the spilling
            if (!Operand.IsMemory())
              Operand.SetToRegister();
            else
              Operand.SetVirtual(false);

            auto Load = MachineInstruction(MachineInstruction::LOAD, &BB);
            /// NOTE: 3 register left in pool using the 2nd and 3rd for uses
            OperandSave.SetToRegister();
            Load.AddOperand(OperandSave);
            Load.AddStackAccess(UsedReg);
            TM->SelectInstruction(&Load);

            BB.InsertInstr(std::move(Load), i++);
          }

          /// Insert STORE if needed
          auto &Operand = Instructions[i].GetOperands()[0];
          auto DefReg = Operand.GetReg();

          if (RegistersToSpill.count(Operand) == 0)
            continue;

          auto FoundReg =
              GetMatchingSizedRegFromSubRegs(RegisterPool[0],
                                             Operand.GetSize(), TM);
          assert(FoundReg != ~0u && "Cannot found matching sized register");

          Operand.SetReg(FoundReg);

          auto OperandSave = Operand;

          if (!Operand.IsMemory())
            Operand.SetToRegister();
          else
            Operand.SetVirtual(false);

          auto Store = MachineInstruction(MachineInstruction::STORE, &BB);
          Store.AddStackAccess(DefReg);
          /// NOTE: 3 register left in pool using the 1st for def
          OperandSave.SetToRegister();
          Store.AddOperand(OperandSave);
          TM->SelectInstruction(&Store);

          BB.InsertInstr(std::move(Store), ++i);
        }
      }
    }

  // FIXME: Move this out from here and make it a PostRA pass
  // After RA lower the stack accessing operands to their final form
  // based on the final stack frame
    for (auto &BB : Func.GetBasicBlocks())
      for (auto &Instr : BB.GetInstructions()) {
        // Check the operands
        for (auto &Operand : Instr.GetOperands()) {
          // Only interested in memory accessing operands
          if (!Operand.IsStackAccess() && !Operand.IsMemory())
            continue;

          // Handle stack access
          if (Operand.IsStackAccess()) {
            // Using SP as frame register for simplicity
            // TODO: Add FP register handling if target support it.
            auto FrameReg = TM->GetRegInfo()->GetStackRegister();
            auto Offset = (int)Func.GetStackObjectPosition(Operand.GetSlot())
                          + Operand.GetOffset();

            Instr.RemoveMemOperand();
            Instr.AddRegister(FrameReg, TM->GetPointerSize());
            Instr.AddImmediate(Offset);
          }
          // Handle memory access
          else {
            auto BaseReg = Operand.GetReg();
            // TODO: Investigate when exactly this should be other then 0
            auto Offset = Operand.GetOffset();

            unsigned Reg =
                Operand.IsVirtual() ? AllocatedRegisters[BaseReg] : BaseReg;

            auto RegSize = TM->GetRegInfo()->GetRegister(Reg)->GetBitWidth();
            Instr.RemoveMemOperand();
            Instr.AddRegister(Reg, RegSize);
            Instr.AddImmediate(Offset);
          }

          break; // there should be only at most one stack access / instr
        }
      }
  }
}
