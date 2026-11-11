#include "xprevival/cpu.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace xprevival {
namespace {

constexpr std::uint32_t mask_for(bool operand32) noexcept {
    return operand32 ? 0xFFFFFFFFu : 0x0000FFFFu;
}

constexpr std::uint32_t sign_for(bool operand32) noexcept {
    return operand32 ? 0x80000000u : 0x00008000u;
}

} // namespace

Cpu::Cpu(std::size_t ram_bytes) : memory_(ram_bytes, std::uint8_t{0}) {
    if (ram_bytes < 1024u * 1024u) {
        throw std::invalid_argument("emulated RAM must be at least 1 MiB");
    }
    reset();
}

void Cpu::reset() {
    std::fill(memory_.begin(), memory_.end(), std::uint8_t{0});
    regs_ = {};
    regs_.eflags = 0x00000002;
    halted_ = false;
    last_error_.clear();
    set_reset_state();
}

void Cpu::set_reset_state() {
    // Temporary real-mode reset entry. The firmware mapping and full PC reset
    // state (including the 386 hidden CS cache) will be supplied by the PC model.
    regs_.cs = 0xF000;
    regs_.eip = 0x0000FFF0;
    halted_ = false;
}

void Cpu::load(std::span<const std::uint8_t> bytes, std::uint32_t address) {
    if (address > memory_.size() || bytes.size() > memory_.size() - address) {
        throw std::out_of_range("program does not fit in emulated RAM");
    }
    std::copy(bytes.begin(), bytes.end(), memory_.begin() + address);
}

std::uint32_t Cpu::physical(std::uint16_t segment, std::uint16_t offset) const noexcept {
    // Current execution mode is real mode with a 16-bit code-segment default.
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

std::uint32_t Cpu::read32(std::uint32_t address) const {
    const auto lo = read16(address);
    const auto hi = read16(address + 2);
    return static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
}

void Cpu::write8(std::uint32_t address, std::uint8_t value) {
    if (address >= memory_.size()) throw std::out_of_range("RAM write out of range");
    memory_[address] = value;
}

void Cpu::write16(std::uint32_t address, std::uint16_t value) {
    write8(address, static_cast<std::uint8_t>(value & 0xFF));
    write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

void Cpu::write32(std::uint32_t address, std::uint32_t value) {
    write16(address, static_cast<std::uint16_t>(value & 0xFFFF));
    write16(address + 2, static_cast<std::uint16_t>(value >> 16));
}

std::uint8_t Cpu::fetch8() {
    const auto offset = static_cast<std::uint16_t>(regs_.eip & 0xFFFFu);
    const auto value = read8(physical(regs_.cs, offset));
    regs_.eip = (regs_.eip & 0xFFFF0000u) | ((regs_.eip + 1u) & 0xFFFFu);
    return value;
}

std::uint16_t Cpu::fetch16() {
    const auto lo = fetch8();
    const auto hi = fetch8();
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
}

std::uint32_t Cpu::fetch32() {
    const auto lo = fetch16();
    const auto hi = fetch16();
    return static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
}

std::uint32_t Cpu::read_general(std::uint8_t index, bool operand32) const noexcept {
    std::uint32_t value = 0;
    switch (index & 7u) {
        case 0: value = regs_.eax; break;
        case 1: value = regs_.ecx; break;
        case 2: value = regs_.edx; break;
        case 3: value = regs_.ebx; break;
        case 4: value = regs_.esp; break;
        case 5: value = regs_.ebp; break;
        case 6: value = regs_.esi; break;
        case 7: value = regs_.edi; break;
    }
    return value & mask_for(operand32);
}

void Cpu::write_general(std::uint8_t index, bool operand32, std::uint32_t value) noexcept {
    std::uint32_t* target = nullptr;
    switch (index & 7u) {
        case 0: target = &regs_.eax; break;
        case 1: target = &regs_.ecx; break;
        case 2: target = &regs_.edx; break;
        case 3: target = &regs_.ebx; break;
        case 4: target = &regs_.esp; break;
        case 5: target = &regs_.ebp; break;
        case 6: target = &regs_.esi; break;
        case 7: target = &regs_.edi; break;
    }
    if (operand32) *target = value;
    else *target = (*target & 0xFFFF0000u) | (value & 0xFFFFu);
}

bool Cpu::decode_register_modrm(std::uint8_t& reg, std::uint8_t& rm) {
    const auto modrm = fetch8();
    if ((modrm & 0xC0u) != 0xC0u) {
        last_error_ = "memory ModRM operands are not implemented yet";
        halted_ = true;
        return false;
    }
    reg = static_cast<std::uint8_t>((modrm >> 3) & 7u);
    rm = static_cast<std::uint8_t>(modrm & 7u);
    return true;
}

void Cpu::set_szp(std::uint32_t result, bool operand32) noexcept {
    const auto masked = result & mask_for(operand32);
    regs_.eflags &= ~(Zero | Sign | Parity);
    if (masked == 0) regs_.eflags |= Zero;
    if ((masked & sign_for(operand32)) != 0) regs_.eflags |= Sign;
    if ((std::popcount(static_cast<std::uint8_t>(masked & 0xFFu)) % 2) == 0) regs_.eflags |= Parity;
}

std::uint32_t Cpu::add(std::uint32_t lhs, std::uint32_t rhs, bool operand32) noexcept {
    const std::uint64_t mask = mask_for(operand32);
    const std::uint64_t a = lhs & mask;
    const std::uint64_t b = rhs & mask;
    const std::uint64_t wide = a + b;
    const auto result = static_cast<std::uint32_t>(wide & mask);
    const auto sign = sign_for(operand32);

    regs_.eflags &= ~(Carry | AuxiliaryCarry | Overflow);
    if (wide > mask) regs_.eflags |= Carry;
    if (((lhs ^ rhs ^ result) & 0x10u) != 0) regs_.eflags |= AuxiliaryCarry;
    if (((~(lhs ^ rhs) & (lhs ^ result)) & sign) != 0) regs_.eflags |= Overflow;
    set_szp(result, operand32);
    return result;
}

std::uint32_t Cpu::subtract(std::uint32_t lhs, std::uint32_t rhs, bool operand32) noexcept {
    const std::uint64_t mask = mask_for(operand32);
    const std::uint64_t a = lhs & mask;
    const std::uint64_t b = rhs & mask;
    const auto result = static_cast<std::uint32_t>((a - b) & mask);
    const auto sign = sign_for(operand32);

    regs_.eflags &= ~(Carry | AuxiliaryCarry | Overflow);
    if (a < b) regs_.eflags |= Carry;
    if (((lhs ^ rhs ^ result) & 0x10u) != 0) regs_.eflags |= AuxiliaryCarry;
    if ((((lhs ^ rhs) & (lhs ^ result)) & sign) != 0) regs_.eflags |= Overflow;
    set_szp(result, operand32);
    return result;
}

void Cpu::logic_flags(std::uint32_t result, bool operand32) noexcept {
    regs_.eflags &= ~(Carry | Overflow);
    set_szp(result, operand32);
}

bool Cpu::condition(std::uint8_t code) const noexcept {
    const bool cf = (regs_.eflags & Carry) != 0;
    const bool pf = (regs_.eflags & Parity) != 0;
    const bool zf = (regs_.eflags & Zero) != 0;
    const bool sf = (regs_.eflags & Sign) != 0;
    const bool of = (regs_.eflags & Overflow) != 0;
    switch (code & 0x0Fu) {
        case 0x0: return of;
        case 0x1: return !of;
        case 0x2: return cf;
        case 0x3: return !cf;
        case 0x4: return zf;
        case 0x5: return !zf;
        case 0x6: return cf || zf;
        case 0x7: return !cf && !zf;
        case 0x8: return sf;
        case 0x9: return !sf;
        case 0xA: return pf;
        case 0xB: return !pf;
        case 0xC: return sf != of;
        case 0xD: return sf == of;
        case 0xE: return zf || (sf != of);
        case 0xF: return !zf && (sf == of);
    }
    return false;
}

void Cpu::push(std::uint32_t value, bool operand32) {
    auto sp = static_cast<std::uint16_t>(regs_.esp & 0xFFFFu);
    sp = static_cast<std::uint16_t>(sp - (operand32 ? 4u : 2u));
    regs_.esp = (regs_.esp & 0xFFFF0000u) | sp;
    const auto address = physical(regs_.ss, sp);
    if (operand32) write32(address, value);
    else write16(address, static_cast<std::uint16_t>(value));
}

std::uint32_t Cpu::pop(bool operand32) {
    const auto sp = static_cast<std::uint16_t>(regs_.esp & 0xFFFFu);
    const auto address = physical(regs_.ss, sp);
    const auto value = operand32 ? read32(address) : read16(address);
    const auto next = static_cast<std::uint16_t>(sp + (operand32 ? 4u : 2u));
    regs_.esp = (regs_.esp & 0xFFFF0000u) | next;
    return value;
}

void Cpu::step() {
    if (halted_) return;

    bool operand32 = false;
    std::uint8_t opcode = fetch8();
    // In a 16-bit code segment, 66h selects 32-bit operand size. Repeated
    // 66h prefixes have the same effect; 67h/address-size is not yet supported.
    std::size_t prefix_count = 0;
    while (opcode == 0x66 && prefix_count < 15) {
        operand32 = true;
        ++prefix_count;
        opcode = fetch8();
    }
    if (prefix_count >= 15) {
        last_error_ = "instruction exceeds the 15-byte x86 length limit";
        halted_ = true;
        return;
    }

    if (opcode == 0x90) return; // NOP
    if (opcode == 0xF4) { halted_ = true; return; } // HLT

    if (opcode >= 0xB8 && opcode <= 0xBF) {
        const auto value = operand32 ? fetch32() : fetch16();
        write_general(static_cast<std::uint8_t>(opcode - 0xB8), operand32, value);
        return;
    }

    if (opcode >= 0x40 && opcode <= 0x47) {
        const auto index = static_cast<std::uint8_t>(opcode - 0x40);
        const bool carry = (regs_.eflags & Carry) != 0;
        const auto result = add(read_general(index, operand32), 1, operand32);
        write_general(index, operand32, result);
        if (carry) regs_.eflags |= Carry; else regs_.eflags &= ~Carry;
        return;
    }
    if (opcode >= 0x48 && opcode <= 0x4F) {
        const auto index = static_cast<std::uint8_t>(opcode - 0x48);
        const bool carry = (regs_.eflags & Carry) != 0;
        const auto result = subtract(read_general(index, operand32), 1, operand32);
        write_general(index, operand32, result);
        if (carry) regs_.eflags |= Carry; else regs_.eflags &= ~Carry;
        return;
    }

    if (opcode >= 0x50 && opcode <= 0x57) {
        push(read_general(static_cast<std::uint8_t>(opcode - 0x50), operand32), operand32);
        return;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        write_general(static_cast<std::uint8_t>(opcode - 0x58), operand32, pop(operand32));
        return;
    }

    if (opcode >= 0x70 && opcode <= 0x7F) {
        const auto displacement = static_cast<std::int8_t>(fetch8());
        if (condition(static_cast<std::uint8_t>(opcode & 0x0Fu))) {
            const auto target = static_cast<std::uint16_t>((regs_.eip + displacement) & 0xFFFFu);
            regs_.eip = (regs_.eip & 0xFFFF0000u) | target;
        }
        return;
    }

    if (opcode == 0xEB) {
        const auto displacement = static_cast<std::int8_t>(fetch8());
        const auto target = static_cast<std::uint16_t>((regs_.eip + displacement) & 0xFFFFu);
        regs_.eip = (regs_.eip & 0xFFFF0000u) | target;
        return;
    }

    if (opcode == 0xE9 || opcode == 0xE8) {
        const std::int32_t displacement = operand32
            ? static_cast<std::int32_t>(fetch32())
            : static_cast<std::int16_t>(fetch16());
        if (opcode == 0xE8) push(regs_.eip & (operand32 ? 0xFFFFFFFFu : 0xFFFFu), operand32);
        if (operand32) regs_.eip += static_cast<std::uint32_t>(displacement);
        else regs_.eip = (regs_.eip & 0xFFFF0000u) |
                         (static_cast<std::uint16_t>((regs_.eip & 0xFFFFu) + displacement));
        return;
    }

    if (opcode == 0xC3) {
        const auto target = pop(operand32);
        if (operand32) regs_.eip = target;
        else regs_.eip = (regs_.eip & 0xFFFF0000u) | (target & 0xFFFFu);
        return;
    }

    if (opcode == 0x05 || opcode == 0x2D || opcode == 0x3D) {
        const auto immediate = operand32 ? fetch32() : fetch16();
        const auto lhs = read_general(0, operand32);
        if (opcode == 0x05) write_general(0, operand32, add(lhs, immediate, operand32));
        else if (opcode == 0x2D) write_general(0, operand32, subtract(lhs, immediate, operand32));
        else (void)subtract(lhs, immediate, operand32);
        return;
    }

    if (opcode == 0x89 || opcode == 0x8B || opcode == 0x01 || opcode == 0x03 ||
        opcode == 0x29 || opcode == 0x2B || opcode == 0x31 || opcode == 0x33 ||
        opcode == 0x39 || opcode == 0x3B) {
        std::uint8_t reg = 0, rm = 0;
        if (!decode_register_modrm(reg, rm)) return;
        const auto reg_value = read_general(reg, operand32);
        const auto rm_value = read_general(rm, operand32);
        switch (opcode) {
            case 0x89: write_general(rm, operand32, reg_value); break;
            case 0x8B: write_general(reg, operand32, rm_value); break;
            case 0x01: write_general(rm, operand32, add(rm_value, reg_value, operand32)); break;
            case 0x03: write_general(reg, operand32, add(reg_value, rm_value, operand32)); break;
            case 0x29: write_general(rm, operand32, subtract(rm_value, reg_value, operand32)); break;
            case 0x2B: write_general(reg, operand32, subtract(reg_value, rm_value, operand32)); break;
            case 0x31: { const auto v = rm_value ^ reg_value; write_general(rm, operand32, v); logic_flags(v, operand32); break; }
            case 0x33: { const auto v = reg_value ^ rm_value; write_general(reg, operand32, v); logic_flags(v, operand32); break; }
            case 0x39: (void)subtract(rm_value, reg_value, operand32); break;
            case 0x3B: (void)subtract(reg_value, rm_value, operand32); break;
        }
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
