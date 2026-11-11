#include "xprevival/cpu.hpp"

#include <stdexcept>

namespace xprevival {

Cpu::Cpu(std::size_t ram_bytes) : memory_(ram_bytes, 0) {
    if (ram_bytes < 1024u * 1024u) {
        throw std::invalid_argument("emulated RAM must be at least 1 MiB");
    }
    reset();
}

void Cpu::reset() {
    std::fill(memory_.begin(), memory_.end(), 0);
    regs_ = {};
    regs_.flags = 0x0002;
    halted_ = false;
    last_error_.clear();
    set_reset_state();
}

void Cpu::set_reset_state() {
    // Temporary real-mode reset entry. A future PC model will map firmware
    // into the top of the physical address space and provide the reset vector.
    regs_.cs = 0xF000;
    regs_.ip = 0xFFF0;
    halted_ = false;
}

void Cpu::load(std::span<const std::uint8_t> bytes, std::uint32_t address) {
    if (address > memory_.size() || bytes.size() > memory_.size() - address) {
        throw std::out_of_range("program does not fit in emulated RAM");
    }
    std::copy(bytes.begin(), bytes.end(), memory_.begin() + address);
}

std::uint32_t Cpu::physical(std::uint16_t segment, std::uint16_t offset) const noexcept {
    // The current interpreter is still real-mode only. A20 and protected-mode
    // address translation are not implemented yet.
    return (static_cast<std::uint32_t>(segment) << 4) + offset;
}

std::uint8_t Cpu::read8(std::uint32_t address) const {
    if (address >= memory_.size()) throw std::out_of_range("RAM read out of range");
    return memory_[address];
}

std::uint16_t Cpu::read16(std::uint32_t address) const {
    const auto lo = read8(address);
    const auto hi = read8(address + 1);
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
}

void Cpu::write8(std::uint32_t address, std::uint8_t value) {
    if (address >= memory_.size()) throw std::out_of_range("RAM write out of range");
    memory_[address] = value;
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

    constexpr char digits[] = "0123456789ABCDEF";
    last_error_ = "unsupported opcode 0x";
    last_error_ += digits[(opcode >> 4) & 0xF];
    last_error_ += digits[opcode & 0xF];
    halted_ = true;
}

void Cpu::run(std::size_t instruction_limit) {
    for (std::size_t i = 0; i < instruction_limit && !halted_; ++i) step();
}

} // namespace xprevival
