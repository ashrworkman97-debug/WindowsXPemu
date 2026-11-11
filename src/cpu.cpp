#include "xprevival/cpu.hpp"

#include <stdexcept>

namespace xprevival {

Cpu::Cpu() { reset(); }

void Cpu::reset() {
    memory_.fill(0);
    regs_ = {};
    regs_.flags = 0x0002;
    halted_ = false;
    last_error_.clear();
    set_reset_state();
}

void Cpu::set_reset_state() {
    // 8086-compatible reset entry point. The full PC model will later map
    // firmware at this address; this core starts at F000:FFF0.
    regs_.cs = 0xF000;
    regs_.ip = 0xFFF0;
    halted_ = false;
}

void Cpu::load(std::span<const std::uint8_t> bytes, std::uint32_t address) {
    if (address >= memory_.size() || bytes.size() > memory_.size() - address) {
        throw std::out_of_range("program does not fit in emulated memory");
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        memory_[address + i] = bytes[i];
    }
}

std::uint32_t Cpu::physical(std::uint16_t segment, std::uint16_t offset) const noexcept {
    return ((static_cast<std::uint32_t>(segment) << 4) + offset) & 0xFFFFF;
}

std::uint8_t Cpu::read8(std::uint32_t address) const {
    return memory_[address & 0xFFFFF];
}

std::uint16_t Cpu::read16(std::uint32_t address) const {
    const auto lo = read8(address);
    const auto hi = read8(address + 1);
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
}

void Cpu::write8(std::uint32_t address, std::uint8_t value) {
    memory_[address & 0xFFFFF] = value;
}

void Cpu::write16(std::uint32_t address, std::uint16_t value) {
    write8(address, static_cast<std::uint8_t>(value & 0xFF));
    write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

std::uint8_t Cpu::fetch8() {
    const auto value = read8(physical(regs_.cs, regs_.ip));
    regs_.ip = static_cast<std::uint16_t>(regs_.ip + 1);
    return value;
}

std::uint16_t Cpu::fetch16() {
    const auto lo = fetch8();
    const auto hi = fetch8();
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
}

void Cpu::set_logic_flags(std::uint16_t value) noexcept {
    regs_.flags &= static_cast<std::uint16_t>(~(Zero | Sign | Parity | Carry | Overflow));
    if (value == 0) regs_.flags |= Zero;
    if ((value & 0x8000) != 0) regs_.flags |= Sign;
    auto low = static_cast<std::uint8_t>(value & 0xFF);
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += (low >> i) & 1u;
    if ((ones & 1u) == 0) regs_.flags |= Parity;
}

void Cpu::step() {
    if (halted_) return;
    const auto opcode = fetch8();

    // Initial real-mode subset: NOP, HLT, MOV r16,imm16, and short JMP.
    if (opcode == 0x90) return;
    if (opcode == 0xF4) { halted_ = true; return; }

    if (opcode >= 0xB8 && opcode <= 0xBF) {
        const auto value = fetch16();
        switch (opcode - 0xB8) {
            case 0: regs_.ax = value; break;
            case 1: regs_.cx = value; break;
            case 2: regs_.dx = value; break;
            case 3: regs_.bx = value; break;
            case 4: regs_.sp = value; break;
            case 5: regs_.bp = value; break;
            case 6: regs_.si = value; break;
            case 7: regs_.di = value; break;
        }
        return;
    }

    if (opcode == 0xEB) {
        const auto displacement = static_cast<std::int8_t>(fetch8());
        regs_.ip = static_cast<std::uint16_t>(regs_.ip + displacement);
        return;
    }

    last_error_ = "unsupported opcode 0x";
    constexpr char digits[] = "0123456789ABCDEF";
    last_error_ += digits[(opcode >> 4) & 0xF];
    last_error_ += digits[opcode & 0xF];
    halted_ = true;
}

void Cpu::run(std::size_t instruction_limit) {
    for (std::size_t i = 0; i < instruction_limit && !halted_; ++i) step();
}

} // namespace xprevival
