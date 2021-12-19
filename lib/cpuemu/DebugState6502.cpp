/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "apple2tc/DebugState6502.h"

#include "apple2tc/a2symbols.h"

/// Reset all debugging.
void DebugState6502::reset() {
  setCollect(nullptr, false);
  setDebugBB(false);
  setBuffering(false);
  setLimit(0);
  clearHistory();
  clearWatches();
  resetCollectedData();
}

void DebugState6502::setBuffering(bool buffering) {
  if (!buffering && buffering_) {
    history_.clear();
    history_.shrink_to_fit();
  }
  buffering_ = buffering;
}

void DebugState6502::setMaxHistory(unsigned maxHistory) {
  if (maxHistory < maxHistory_) {
    history_.resize(maxHistory);
    history_.shrink_to_fit();
  }
  maxHistory_ = maxHistory;
}

void DebugState6502::clearHistory() {
  history_.clear();
}

void DebugState6502::printHistory() {
  for (const auto &rec : history_) {
    printRecord(rec, true);
    printf("\n");
  }
}

void DebugState6502::addWatch(std::string name, uint16_t addr, uint8_t size) {
  auto it = std::find_if(watches_.begin(), watches_.end(), [addr, size](const Watch &w) {
    return w.addr == addr && w.size == size;
  });

  if (it != watches_.end()) {
    it->name = name;
  } else {
    watches_.emplace_back(std::move(name), addr, size);
  }
}

void DebugState6502::removeWatch(const char *name) {
  auto it = std::find_if(
      watches_.begin(), watches_.end(), [&name](const Watch &w) { return w.name == name; });
  if (it != watches_.end())
    watches_.erase(it);
}

void DebugState6502::addNonDebug(uint16_t from, uint16_t to) {
  nonDebug_.emplace_back(from, to);
}

Emu6502::StopReason DebugState6502::debugStateCB(void *ctx, Emu6502 *emu, uint16_t pc) {
  return static_cast<DebugState6502 *>(ctx)->debugState(emu, pc);
}

void DebugState6502::printRecord(const InstRecord &rec, bool showInst) {
  auto r = rec.regs;
  // Address.
  {
    const char *name = resolveApple2Symbols_ ? findApple2Symbol(r.pc) : nullptr;
    printf("%04X: %-8s  ", r.pc, name ? name : "");
  }

  // Dump the registers.
  printf("A=%02X X=%02X Y=%02X SP=%02X SR=", r.a, r.x, r.y, r.sp);
  // Dump the flags.
  static const char names[9] = "NV.BDIZC";
  for (unsigned i = 0; i != 8; ++i)
    putchar((r.status & (0x80 >> i)) ? names[i] : '.');

  if (showInst) {
    // The PC again for convenience.
    printf(" PC=%04X  ", r.pc);
    // Dump the next instruction.
    ThreeBytes bytes = rec.bytes;
    auto inst = decodeInst(r.pc, bytes);
    auto fmt = resolveApple2Symbols_ ? formatInst(inst, bytes, apple2SymbolResolver)
                                     : formatInst(inst, bytes);
    printf("  %-8s    %s", fmt.bytes, fmt.inst);
    if (fmt.operand[0]) {
      printf("  %s", fmt.operand.c_str());
      if (inst.addrMode == CPUAddrMode::Rel)
        printf(" (%d)", (int8_t)bytes.d[1]);
    }
  }
}

void DebugState6502::addRecord(const InstRecord &rec) {
  if (history_.size() >= maxHistory_)
    history_.pop_front();
  history_.push_back(rec);
}

static inline ThreeBytes ram_peek3(const Emu6502 *emu, uint16_t addr) {
  ThreeBytes b;
  b.d[0] = emu->ram_peek(addr);
  b.d[1] = emu->ram_peek(addr + 1);
  b.d[2] = emu->ram_peek(addr + 2);
  return b;
}

Emu6502::StopReason DebugState6502::debugState(Emu6502 *emu, uint16_t pc) {
  // Don't debug in areas that have been excluded.
  for (auto p : nonDebug_) {
    if (pc >= p.first && pc <= p.second)
      return Emu6502::StopReason::None;
  }

  if (collect_)
    return collectData(emu, pc);

  if (debugBB_) {
    bool wasBranchTarget = branchTarget_;
    CPUOpcode opc = decodeOpcode(emu->ram_peek(pc));
    branchTarget_ = instIsBranch(opc.kind, opc.addrMode);
    // If the current instruction was not a branch, leave.
    if (!wasBranchTarget)
      return Emu6502::StopReason::None;
  }

  if (limit_ && icount_ >= limit_)
    return Emu6502::StopReason::StopRequesed;
  ++icount_;

  Emu6502::Regs r = emu->getRegs();
  InstRecord rec = {.regs = r, .bytes = ram_peek3(emu, pc)};

  if (buffering_) {
    addRecord(rec);

    if (decodeOpcode(rec.bytes.d[0]).kind == CPUInstKind::INVALID) {
      printf("*** INVALID OPCODE! Dumping history:\n");
      printHistory();
      return Emu6502::StopReason::StopRequesed;
    }

    return Emu6502::StopReason::None;
  }

  printRecord(rec, watches_.empty());

  // Dump watches
  for (const auto &watch : watches_) {
    putchar(' ');
    if (!watch.name.empty())
      printf("%s", watch.name.c_str());
    if (watch.addr < 256)
      printf("($%02X)=", watch.addr);
    else
      printf("($%04X)=", watch.addr);
    if (watch.size == 1)
      printf("$%02X", emu->ram_peek(watch.addr));
    else
      printf("$%04X", emu->ram_peek16(watch.addr));
  }
  putchar('\n');

  return Emu6502::StopReason::None;
}

void DebugState6502::setCollect(Emu6502 *emu, bool on) {
  if (on && !collect_) {
    curMemWritten_.clear();
    prevMemWritten_.clear();
    curMemExec_.clear();
    generations_.clear();
    generations_.emplace_back();
    generations_.back().regs = emu->getRegs();
  }
  collect_ = on;
}

/// Calculate the effective address that would be operated on by the instruction.
/// If the instruction doesn't access memory, just return 0, so it is always safe
/// to call this function.
static uint16_t
operandEA(const Emu6502 *emu, Emu6502::Regs regs, CPUAddrMode am, uint16_t operand) {
  switch (am) {
  case CPUAddrMode::Abs:
  case CPUAddrMode::Rel:
  case CPUAddrMode::Zpg:
    return operand;

  case CPUAddrMode::Abs_X:
    return operand + regs.x;
  case CPUAddrMode::Abs_Y:
    return operand + regs.y;
  case CPUAddrMode::Ind:
    return emu->ram_peek16(operand);
  case CPUAddrMode::X_Ind:
    return emu->ram_peek16((operand + regs.x) & 0xFF);
  case CPUAddrMode::Ind_Y:
    return emu->ram_peek16(operand & 0xFF) + regs.y;
  case CPUAddrMode::Zpg_X:
    return (operand + regs.x) & 255;
  case CPUAddrMode::Zpg_Y:
    return (operand + regs.y) & 255;

  default:
    return 0;
  }
}

Emu6502::StopReason DebugState6502::collectData(const Emu6502 *emu, uint16_t pc) {
  ThreeBytes b = ram_peek3(emu, pc);
  CPUInst inst = decodeInst(pc, b);
  Emu6502::Regs regs = emu->getRegs();
  uint16_t ea = operandEA(emu, regs, inst.addrMode, inst.operand);

  if (curMemWritten_.get(pc)) {
    // Are we executing an opcode that was modified in the current generation?
    // If so, start a new generation.
    newGeneration(emu, regs);
    curMemExec_.setMulti(pc, pc + inst.size, true);
  } else if (prevMemWritten_.get(pc)) {
    // We are executing something that was written in the previous generation.
    // Record the range of the entire instruction.
    curMemExec_.setMulti(pc, pc + inst.size, true);
  }

  if (instIsBranch(inst.kind, inst.addrMode)) {
    branchTargets_.insert(ea);
    if (limit_ && icount_ >= limit_)
      return Emu6502::StopReason::StopRequesed;
    ++icount_;
  } else if (instWritesMemNormal(inst.kind, inst.addrMode)) {
    curMemWritten_.set(ea, true);
  }
  return Emu6502::StopReason::None;
}

void DebugState6502::newGeneration(const Emu6502 *emu, Emu6502::Regs regs) {
  generations_.emplace_back();
  auto &gen = generations_.back();
  gen.regs = regs;

  unsigned from = 0;
  while ((from = curMemExec_.findSetBit(from)) != curMemExec_.size()) {
    unsigned to = curMemExec_.findClearBit(from + 1);
    gen.addRange(from, to - from, emu->getMainRAM() + from);
    if (to == curMemExec_.size())
      break;
    from = to + 1;
  }

  fprintf(stderr, "Saved %zu bytes to previous generation\n", gen.data.size());

  curMemExec_.clear();
  prevMemWritten_.swap(curMemWritten_);
  curMemWritten_.clear();
}

void DebugState6502::resetCollectedData() {
  branchTargets_.clear();
  generations_.clear();
}
