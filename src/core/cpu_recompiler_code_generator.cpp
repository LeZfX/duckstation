#include "cpu_recompiler_code_generator.h"
#include "YBaseLib/Log.h"
#include "cpu_disasm.h"
Log_SetChannel(CPU::Recompiler);

namespace CPU::Recompiler {

CodeGenerator::CodeGenerator(Core* cpu, JitCodeBuffer* code_buffer, const ASMFunctions& asm_functions)
  : m_cpu(cpu), m_code_buffer(code_buffer), m_asm_functions(asm_functions), m_register_cache(*this),
    m_emit(code_buffer->GetFreeCodeSpace(), code_buffer->GetFreeCodePointer())
{
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() {}

u32 CodeGenerator::CalculateRegisterOffset(Reg reg)
{
  return uint32(offsetof(Core, m_regs.r[0]) + (static_cast<u32>(reg) * sizeof(u32)));
}

bool CodeGenerator::CompileBlock(const CodeBlock* block, void** out_function_ptr, size_t* out_code_size)
{
  // TODO: Align code buffer.

  m_block = block;
  m_block_start = block->instructions.data();
  m_block_end = block->instructions.data() + block->instructions.size();

  m_current_instruction_in_branch_delay_slot_dirty = true;
  m_branch_was_taken_dirty = true;
  m_current_instruction_was_branch_taken_dirty = false;
  m_load_delay_dirty = true;

  EmitBeginBlock();
  BlockPrologue();

  const CodeBlockInstruction* cbi = m_block_start;
  while (cbi != m_block_end)
  {
#ifndef Y_BUILD_CONFIG_RELEASE
    SmallString disasm;
    DisassembleInstruction(&disasm, cbi->pc, cbi->instruction.bits, nullptr);
    Log_DebugPrintf("Compiling instruction '%s'", disasm.GetCharArray());
#endif

    if (!CompileInstruction(*cbi))
    {
      m_block_end = nullptr;
      m_block_start = nullptr;
      m_block = nullptr;
      return false;
    }

    cbi++;
  }

  BlockEpilogue();
  EmitEndBlock();

  FinalizeBlock(out_function_ptr, out_code_size);

  DebugAssert(m_register_cache.GetUsedHostRegisters() == 0);

  m_block_end = nullptr;
  m_block_start = nullptr;
  m_block = nullptr;
  return true;
}

bool CodeGenerator::CompileInstruction(const CodeBlockInstruction& instruction)
{
  bool result;
  switch (instruction.instruction.op)
  {
#if 1
    case InstructionOp::lui:
      result = Compile_lui(instruction);
      break;
#endif

    default:
      result = Compile_Fallback(instruction);
      break;
  }

  // release temporary effective addresses
  for (Value& value : m_operand_memory_addresses)
    value.ReleaseAndClear();

  return result;
}

Value CodeGenerator::ConvertValueSize(const Value& value, RegSize size, bool sign_extend)
{
  DebugAssert(value.size != size);

  if (value.IsConstant())
  {
    // compile-time conversion, woo!
    switch (size)
    {
      case RegSize_8:
        return Value::FromConstantU8(value.constant_value & 0xFF);

      case RegSize_16:
      {
        switch (value.size)
        {
          case RegSize_8:
            return Value::FromConstantU16(sign_extend ? SignExtend16(Truncate8(value.constant_value)) :
                                                        ZeroExtend16(Truncate8(value.constant_value)));

          default:
            return Value::FromConstantU16(value.constant_value & 0xFFFF);
        }
      }
      break;

      case RegSize_32:
      {
        switch (value.size)
        {
          case RegSize_8:
            return Value::FromConstantU32(sign_extend ? SignExtend32(Truncate8(value.constant_value)) :
                                                        ZeroExtend32(Truncate8(value.constant_value)));
          case RegSize_16:
            return Value::FromConstantU32(sign_extend ? SignExtend32(Truncate16(value.constant_value)) :
                                                        ZeroExtend32(Truncate16(value.constant_value)));

          case RegSize_32:
            return value;

          default:
            break;
        }
      }
      break;

      default:
        break;
    }

    UnreachableCode();
    return Value{};
  }

  Value new_value = m_register_cache.AllocateScratch(size);
  if (size < value.size)
  {
    EmitCopyValue(new_value.host_reg, value);
  }
  else
  {
    if (sign_extend)
      EmitSignExtend(new_value.host_reg, size, value.host_reg, value.size);
    else
      EmitZeroExtend(new_value.host_reg, size, value.host_reg, value.size);
  }

  return new_value;
}

void CodeGenerator::ConvertValueSizeInPlace(Value* value, RegSize size, bool sign_extend)
{
  DebugAssert(value->size != size);

  // We don't want to mess up the register cache value, so generate a new value if it's not scratch.
  if (value->IsConstant() || !value->IsScratch())
  {
    *value = ConvertValueSize(*value, size, sign_extend);
    return;
  }

  DebugAssert(value->IsInHostRegister() && value->IsScratch());

  // If the size is smaller and the value is in a register, we can just "view" the lower part.
  if (size < value->size)
  {
    value->size = size;
  }
  else
  {
    if (sign_extend)
      EmitSignExtend(value->host_reg, size, value->host_reg, value->size);
    else
      EmitZeroExtend(value->host_reg, size, value->host_reg, value->size);
  }

  value->size = size;
}

Value CodeGenerator::AddValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value + rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  EmitCopyValue(res.host_reg, lhs);
  EmitAdd(res.host_reg, rhs);
  return res;
}

Value CodeGenerator::ShlValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value << rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  EmitCopyValue(res.host_reg, lhs);
  EmitShl(res.host_reg, res.size, rhs);
  return res;
}

void CodeGenerator::BlockPrologue()
{
  EmitStoreCPUStructField(offsetof(Core, m_exception_raised), Value::FromConstantU8(0));
}

void CodeGenerator::BlockEpilogue()
{
  m_register_cache.FlushAllGuestRegisters(true, false);
  SyncInstructionPointer();

  // TODO: correct value for is_branch_delay_slot - branches in branch delay slot.
  EmitStoreCPUStructField(offsetof(Core, m_next_instruction_is_branch_delay_slot), Value::FromConstantU8(0));
}

void CodeGenerator::InstructionPrologue(const CodeBlockInstruction& cbi, TickCount cycles,
                                        bool force_sync /* = false */)
{
  // reset dirty flags
  if (m_branch_was_taken_dirty)
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_8);
    EmitLoadCPUStructField(temp.host_reg, RegSize_8, offsetof(Core, m_branch_was_taken));
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_was_branch_taken), temp);
    EmitStoreCPUStructField(offsetof(Core, m_branch_was_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = true;
    m_branch_was_taken_dirty = false;
  }
  else if (m_current_instruction_was_branch_taken_dirty)
  {
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_was_branch_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = false;
  }

  if (m_current_instruction_in_branch_delay_slot_dirty && !cbi.is_branch_delay_slot)
  {
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_in_branch_delay_slot), Value::FromConstantU8(0));
    m_current_instruction_in_branch_delay_slot_dirty = false;
  }

  if (!CanInstructionTrap(cbi.instruction, m_block->key.user_mode) && !force_sync)
  {
    // Defer updates for non-faulting instructions.
    m_delayed_pc_add += INSTRUCTION_SIZE;
    m_delayed_cycles_add += cycles;
    return;
  }

  if (cbi.is_branch_delay_slot)
  {
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_in_branch_delay_slot), Value::FromConstantU8(1));
    m_current_instruction_in_branch_delay_slot_dirty = true;
  }

  // m_current_instruction_pc = m_regs.pc
  {
    Value pc_value = m_register_cache.AllocateScratch(RegSize_32);
    EmitLoadCPUStructField(pc_value.host_reg, RegSize_32, offsetof(Core, m_regs.pc));
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_pc), pc_value);
  }

  // m_regs.pc = m_regs.npc
  {
    Value npc_value = m_register_cache.AllocateScratch(RegSize_32);
    EmitLoadCPUStructField(npc_value.host_reg, RegSize_32, offsetof(Core, m_regs.npc));
    EmitStoreCPUStructField(offsetof(Core, m_regs.pc), npc_value);
  }

  // m_regs.npc += INSTRUCTION_SIZE
  EmitAddCPUStructField(offsetof(Core, m_regs.npc), Value::FromConstantU32(m_delayed_pc_add + INSTRUCTION_SIZE));
  m_delayed_pc_add = 0;

  // Add pending cycles for this instruction.
  EmitAddCPUStructField(offsetof(Core, m_pending_ticks), Value::FromConstantU32(m_delayed_cycles_add + cycles));
  EmitAddCPUStructField(offsetof(Core, m_downcount), Value::FromConstantU32(~u32(m_delayed_cycles_add + cycles - 1)));
  m_delayed_cycles_add = 0;
}

void CodeGenerator::InstructionEpilogue(const CodeBlockInstruction& cbi)
{
  // copy if the previous instruction was a load, reset the current value on the next instruction
  if (m_load_delay_dirty)
  {
    // cpu->m_load_delay_reg = cpu->m_next_load_delay_reg;
    // cpu->m_next_load_delay_reg = Reg::count;
    {
      Value temp = m_register_cache.AllocateScratch(RegSize_8);
      EmitLoadCPUStructField(temp.host_reg, RegSize_8, offsetof(Core, m_next_load_delay_reg));
      EmitStoreCPUStructField(offsetof(Core, m_next_load_delay_reg),
                              Value::FromConstantU8(static_cast<u8>(Reg::count)));
      EmitStoreCPUStructField(offsetof(Core, m_load_delay_reg), temp);
    }

    // cpu->m_load_delay_old_value = cpu->m_next_load_delay_old_value;
    // cpu->m_next_load_delay_old_value = 0;
    {
      Value temp = m_register_cache.AllocateScratch(RegSize_32);
      EmitLoadCPUStructField(temp.host_reg, RegSize_32, offsetof(Core, m_next_load_delay_old_value));
      EmitStoreCPUStructField(offsetof(Core, m_next_load_delay_old_value), Value::FromConstantU32(0));
      EmitStoreCPUStructField(offsetof(Core, m_load_delay_old_value), temp);
    }

    m_load_delay_dirty = false;
    m_next_load_delay_dirty = true;
  }
  else if (m_next_load_delay_dirty)
  {
    // cpu->m_load_delay_reg = Reg::count;
    // cpu->m_load_delay_old_value = 0;
    EmitStoreCPUStructField(offsetof(Core, m_load_delay_reg), Value::FromConstantU8(static_cast<u8>(Reg::count)));
    EmitStoreCPUStructField(offsetof(Core, m_load_delay_old_value), Value::FromConstantU32(0));

    m_next_load_delay_dirty = false;
  }
}

void CodeGenerator::SyncInstructionPointer()
{
  if (m_delayed_pc_add > 0)
  {
    EmitAddCPUStructField(offsetof(Core, m_regs.npc), Value::FromConstantU32(m_delayed_pc_add));
    m_delayed_pc_add = 0;
  }

  if (m_delayed_cycles_add > 0)
  {
    EmitAddCPUStructField(offsetof(Core, m_pending_ticks), Value::FromConstantU32(m_delayed_cycles_add));
    EmitAddCPUStructField(offsetof(Core, m_downcount), Value::FromConstantU32(~u32(m_delayed_cycles_add - 1)));
    m_delayed_cycles_add = 0;
  }
}

bool CodeGenerator::Compile_Fallback(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1, true);

  // flush and invalidate all guest registers, since the fallback could change any of them
  m_register_cache.FlushAllGuestRegisters(true, true);

  EmitStoreCPUStructField(offsetof(Core, m_current_instruction.bits), Value::FromConstantU32(cbi.instruction.bits));

  // emit the function call
  if (CanInstructionTrap(cbi.instruction, m_block->key.user_mode))
  {
    // TODO: Use carry flag or something here too
    Value return_value = m_register_cache.AllocateScratch(RegSize_8);
    EmitFunctionCall(&return_value, &Thunks::InterpretInstruction, m_register_cache.GetCPUPtr());
    EmitBlockExitOnBool(return_value);
  }
  else
  {
    EmitFunctionCall(nullptr, &Thunks::InterpretInstruction, m_register_cache.GetCPUPtr());
  }

  m_current_instruction_in_branch_delay_slot_dirty = true;
  m_branch_was_taken_dirty = true;
  m_load_delay_dirty = true;
  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_lui(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1, true);

  // rt <- (imm << 16)
  m_register_cache.WriteGuestRegister(cbi.instruction.i.rt,
                                      Value::FromConstantU32(cbi.instruction.i.imm_zext32() << 16));

  InstructionEpilogue(cbi);
  return true;
}

} // namespace CPU::Recompiler