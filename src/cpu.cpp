#include "xprevival/cpu.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace xprevival {
namespace {
constexpr std::uint32_t mask_for(unsigned width) noexcept {
    return width == 32 ? 0xFFFFFFFFu : (width == 16 ? 0x0000FFFFu : 0x000000FFu);
}
constexpr std::uint32_t sign_for(unsigned width) noexcept {
    return width == 32 ? 0x80000000u : (width == 16 ? 0x00008000u : 0x00000080u);
}
}

Cpu::Cpu(std::size_t ram_bytes) : memory_(ram_bytes, std::uint8_t{0}) {
    if (ram_bytes < 1024u * 1024u) throw std::invalid_argument("emulated RAM must be at least 1 MiB");
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
    // Firmware mapping and the 386 hidden CS cache will be provided by the PC model.
    regs_.cs = 0xF000;
    regs_.eip = 0x0000FFF0;
    halted_ = false;
}

void Cpu::load(std::span<const std::uint8_t> bytes, std::uint32_t address) {
    if (address > memory_.size() || bytes.size() > memory_.size() - address)
        throw std::out_of_range("program does not fit in emulated RAM");
    std::copy(bytes.begin(), bytes.end(), memory_.begin() + address);
}

std::uint32_t Cpu::physical(std::uint16_t segment, std::uint16_t offset) const noexcept {
    return (static_cast<std::uint32_t>(segment) << 4) + offset;
}

std::uint8_t Cpu::read8(std::uint32_t address) const {
    if (address >= memory_.size()) throw std::out_of_range("RAM read out of range");
    return memory_[address];
}
std::uint16_t Cpu::read16(std::uint32_t address) const {
    return static_cast<std::uint16_t>(read8(address) | (static_cast<std::uint16_t>(read8(address + 1)) << 8));
}
std::uint32_t Cpu::read32(std::uint32_t address) const {
    return static_cast<std::uint32_t>(read16(address)) | (static_cast<std::uint32_t>(read16(address + 2)) << 16);
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
    const auto value = read8(physical(regs_.cs, static_cast<std::uint16_t>(regs_.eip)));
    regs_.eip = (regs_.eip & 0xFFFF0000u) | ((regs_.eip + 1u) & 0xFFFFu);
    return value;
}
std::uint16_t Cpu::fetch16() {
    const auto lo = fetch8();
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(fetch8()) << 8));
}
std::uint32_t Cpu::fetch32() {
    const auto lo = fetch16();
    return static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(fetch16()) << 16);
}

std::uint32_t Cpu::read_general(std::uint8_t index, bool operand32) const noexcept {
    std::uint32_t value = 0;
    switch (index & 7u) {
        case 0: value = regs_.eax; break; case 1: value = regs_.ecx; break;
        case 2: value = regs_.edx; break; case 3: value = regs_.ebx; break;
        case 4: value = regs_.esp; break; case 5: value = regs_.ebp; break;
        case 6: value = regs_.esi; break; case 7: value = regs_.edi; break;
    }
    return value & mask_for(operand32 ? 32u : 16u);
}
void Cpu::write_general(std::uint8_t index, bool operand32, std::uint32_t value) noexcept {
    std::uint32_t* target = nullptr;
    switch (index & 7u) {
        case 0: target = &regs_.eax; break; case 1: target = &regs_.ecx; break;
        case 2: target = &regs_.edx; break; case 3: target = &regs_.ebx; break;
        case 4: target = &regs_.esp; break; case 5: target = &regs_.ebp; break;
        case 6: target = &regs_.esi; break; case 7: target = &regs_.edi; break;
    }
    if (operand32) *target = value;
    else *target = (*target & 0xFFFF0000u) | (value & 0xFFFFu);
}
std::uint8_t Cpu::read_byte_register(std::uint8_t index) const noexcept {
    const auto word = read_general(static_cast<std::uint8_t>(index & 3u), true);
    return static_cast<std::uint8_t>((index & 4u) ? (word >> 8) : word);
}
void Cpu::write_byte_register(std::uint8_t index, std::uint8_t value) noexcept {
    const auto reg = static_cast<std::uint8_t>(index & 3u);
    const auto current = read_general(reg, true);
    const auto shift = (index & 4u) ? 8u : 0u;
    const auto updated = (current & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift);
    write_general(reg, true, updated);
}

bool Cpu::decode_modrm(Operand& rm, std::uint8_t& reg, bool address32,
                       bool has_segment_override, std::uint16_t segment_override) {
    const auto modrm = fetch8();
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    reg = static_cast<std::uint8_t>((modrm >> 3) & 7u);
    const auto rmi = static_cast<std::uint8_t>(modrm & 7u);
    if (mod == 3) {
        rm = {true, rmi, 0};
        return true;
    }
    if (address32) {
        last_error_ = "32-bit ModRM addressing is not implemented yet";
        halted_ = true;
        return false;
    }

    std::uint32_t base = 0;
    bool uses_bp = false;
    switch (rmi) {
        case 0: base = (regs_.ebx & 0xFFFFu) + (regs_.esi & 0xFFFFu); break;
        case 1: base = (regs_.ebx & 0xFFFFu) + (regs_.edi & 0xFFFFu); break;
        case 2: base = (regs_.ebp & 0xFFFFu) + (regs_.esi & 0xFFFFu); uses_bp = true; break;
        case 3: base = (regs_.ebp & 0xFFFFu) + (regs_.edi & 0xFFFFu); uses_bp = true; break;
        case 4: base = regs_.esi & 0xFFFFu; break;
        case 5: base = regs_.edi & 0xFFFFu; break;
        case 6:
            if (mod == 0) base = 0;
            else { base = regs_.ebp & 0xFFFFu; uses_bp = true; }
            break;
        case 7: base = regs_.ebx & 0xFFFFu; break;
    }

    std::int32_t displacement = 0;
    if (mod == 0 && rmi == 6) displacement = fetch16();
    else if (mod == 1) displacement = static_cast<std::int8_t>(fetch8());
    else if (mod == 2) displacement = static_cast<std::int16_t>(fetch16());

    const auto offset = static_cast<std::uint16_t>(base + displacement);
    const auto segment = has_segment_override ? segment_override : (uses_bp ? regs_.ss : regs_.ds);
    rm = {false, 0, physical(segment, offset)};
    return true;
}

std::uint32_t Cpu::read_operand(const Operand& operand, unsigned width) const {
    if (width == 8) return operand.is_register ? read_byte_register(operand.reg) : read8(operand.address);
    if (width == 16) return operand.is_register ? read_general(operand.reg, false) : read16(operand.address);
    return operand.is_register ? read_general(operand.reg, true) : read32(operand.address);
}
void Cpu::write_operand(const Operand& operand, unsigned width, std::uint32_t value) {
    if (width == 8) {
        if (operand.is_register) write_byte_register(operand.reg, static_cast<std::uint8_t>(value));
        else write8(operand.address, static_cast<std::uint8_t>(value));
    } else if (width == 16) {
        if (operand.is_register) write_general(operand.reg, false, value);
        else write16(operand.address, static_cast<std::uint16_t>(value));
    } else {
        if (operand.is_register) write_general(operand.reg, true, value);
        else write32(operand.address, value);
    }
}

void Cpu::set_szp(std::uint32_t result, unsigned width) noexcept {
    const auto masked = result & mask_for(width);
    regs_.eflags &= ~(Zero | Sign | Parity);
    if (masked == 0) regs_.eflags |= Zero;
    if ((masked & sign_for(width)) != 0) regs_.eflags |= Sign;
    if ((std::popcount(static_cast<std::uint8_t>(masked & 0xFFu)) % 2) == 0) regs_.eflags |= Parity;
}
std::uint32_t Cpu::add(std::uint32_t lhs, std::uint32_t rhs, unsigned width) noexcept {
    const std::uint64_t mask = mask_for(width), a = lhs & mask, b = rhs & mask, wide = a + b;
    const auto result = static_cast<std::uint32_t>(wide & mask), sign = sign_for(width);
    regs_.eflags &= ~(Carry | AuxiliaryCarry | Overflow);
    if (wide > mask) regs_.eflags |= Carry;
    if (((lhs ^ rhs ^ result) & 0x10u) != 0) regs_.eflags |= AuxiliaryCarry;
    if (((~(lhs ^ rhs) & (lhs ^ result)) & sign) != 0) regs_.eflags |= Overflow;
    set_szp(result, width);
    return result;
}
std::uint32_t Cpu::subtract(std::uint32_t lhs, std::uint32_t rhs, unsigned width) noexcept {
    const std::uint64_t mask = mask_for(width), a = lhs & mask, b = rhs & mask;
    const auto result = static_cast<std::uint32_t>((a - b) & mask), sign = sign_for(width);
    regs_.eflags &= ~(Carry | AuxiliaryCarry | Overflow);
    if (a < b) regs_.eflags |= Carry;
    if (((lhs ^ rhs ^ result) & 0x10u) != 0) regs_.eflags |= AuxiliaryCarry;
    if ((((lhs ^ rhs) & (lhs ^ result)) & sign) != 0) regs_.eflags |= Overflow;
    set_szp(result, width);
    return result;
}
void Cpu::logic_flags(std::uint32_t result, unsigned width) noexcept {
    regs_.eflags &= ~(Carry | Overflow);
    regs_.eflags &= ~AuxiliaryCarry;
    set_szp(result, width);
}
bool Cpu::condition(std::uint8_t code) const noexcept {
    const bool cf = (regs_.eflags & Carry) != 0, pf = (regs_.eflags & Parity) != 0;
    const bool zf = (regs_.eflags & Zero) != 0, sf = (regs_.eflags & Sign) != 0;
    const bool of = (regs_.eflags & Overflow) != 0;
    switch (code & 0x0Fu) {
        case 0x0: return of; case 0x1: return !of; case 0x2: return cf; case 0x3: return !cf;
        case 0x4: return zf; case 0x5: return !zf; case 0x6: return cf || zf; case 0x7: return !cf && !zf;
        case 0x8: return sf; case 0x9: return !sf; case 0xA: return pf; case 0xB: return !pf;
        case 0xC: return sf != of; case 0xD: return sf == of; case 0xE: return zf || (sf != of);
        case 0xF: return !zf && (sf == of);
    }
    return false;
}
void Cpu::push(std::uint32_t value, bool operand32) {
    auto sp = static_cast<std::uint16_t>(regs_.esp & 0xFFFFu);
    sp = static_cast<std::uint16_t>(sp - (operand32 ? 4u : 2u));
    regs_.esp = (regs_.esp & 0xFFFF0000u) | sp;
    if (operand32) write32(physical(regs_.ss, sp), value);
    else write16(physical(regs_.ss, sp), static_cast<std::uint16_t>(value));
}
std::uint32_t Cpu::pop(bool operand32) {
    const auto sp = static_cast<std::uint16_t>(regs_.esp & 0xFFFFu);
    const auto address = physical(regs_.ss, sp);
    const auto value = operand32 ? read32(address) : read16(address);
    regs_.esp = (regs_.esp & 0xFFFF0000u) | static_cast<std::uint16_t>(sp + (operand32 ? 4u : 2u));
    return value;
}

void Cpu::step() {
    if (halted_) return;
    bool operand32 = false, address32 = false, has_segment_override = false;
    std::uint16_t segment_override = 0;
    std::uint8_t opcode = fetch8();
    std::size_t prefix_count = 0;
    while (prefix_count < 15) {
        bool consumed = true;
        switch (opcode) {
            case 0x66: operand32 = true; break;
            case 0x67: address32 = true; break;
            case 0x26: segment_override = regs_.es; has_segment_override = true; break;
            case 0x2E: segment_override = regs_.cs; has_segment_override = true; break;
            case 0x36: segment_override = regs_.ss; has_segment_override = true; break;
            case 0x3E: segment_override = regs_.ds; has_segment_override = true; break;
            case 0x64: segment_override = regs_.fs; has_segment_override = true; break;
            case 0x65: segment_override = regs_.gs; has_segment_override = true; break;
            default: consumed = false; break;
        }
        if (!consumed) break;
        ++prefix_count;
        opcode = fetch8();
    }
    if (prefix_count == 15) {
        last_error_ = "instruction exceeds the 15-byte x86 length limit";
        halted_ = true;
        return;
    }

    const auto do_alu = [this](std::uint8_t operation, std::uint32_t lhs,
                               std::uint32_t rhs, unsigned width) {
        switch (operation) {
            case 0: return add(lhs, rhs, width);
            case 1: { const auto v = lhs | rhs; logic_flags(v, width); return v; }
            case 4: { const auto v = lhs & rhs; logic_flags(v, width); return v; }
            case 5: return subtract(lhs, rhs, width);
            case 6: { const auto v = lhs ^ rhs; logic_flags(v, width); return v; }
            case 7: return subtract(lhs, rhs, width); // CMP updates flags, not destination.
            default:
                last_error_ = "ADC/SBB immediate and register forms are not implemented yet";
                halted_ = true;
                return lhs & mask_for(width);
        }
    };

    if (opcode == 0x90) return;
    if (opcode == 0xF4) { halted_ = true; return; }

    if (opcode >= 0xB0 && opcode <= 0xB7) {
        write_byte_register(static_cast<std::uint8_t>(opcode - 0xB0), fetch8());
        return;
    }
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        const auto value = operand32 ? fetch32() : fetch16();
        write_general(static_cast<std::uint8_t>(opcode - 0xB8), operand32, value);
        return;
    }

    if (opcode >= 0x40 && opcode <= 0x47) {
        const auto index = static_cast<std::uint8_t>(opcode - 0x40);
        const bool carry = (regs_.eflags & Carry) != 0;
        const auto result = add(read_general(index, operand32), 1, operand32 ? 32u : 16u);
        write_general(index, operand32, result);
        if (carry) regs_.eflags |= Carry; else regs_.eflags &= ~Carry;
        return;
    }
    if (opcode >= 0x48 && opcode <= 0x4F) {
        const auto index = static_cast<std::uint8_t>(opcode - 0x48);
        const bool carry = (regs_.eflags & Carry) != 0;
        const auto result = subtract(read_general(index, operand32), 1, operand32 ? 32u : 16u);
        write_general(index, operand32, result);
        if (carry) regs_.eflags |= Carry; else regs_.eflags &= ~Carry;
        return;
    }
    if (opcode >= 0x50 && opcode <= 0x57) { push(read_general(static_cast<std::uint8_t>(opcode - 0x50), operand32), operand32); return; }
    if (opcode >= 0x58 && opcode <= 0x5F) { write_general(static_cast<std::uint8_t>(opcode - 0x58), operand32, pop(operand32)); return; }

    if (opcode >= 0x70 && opcode <= 0x7F) {
        const auto displacement = static_cast<std::int8_t>(fetch8());
        if (condition(static_cast<std::uint8_t>(opcode & 0x0Fu))) {
            regs_.eip = (regs_.eip & 0xFFFF0000u) | static_cast<std::uint16_t>(regs_.eip + displacement);
        }
        return;
    }
    if (opcode == 0xEB) {
        const auto displacement = static_cast<std::int8_t>(fetch8());
        regs_.eip = (regs_.eip & 0xFFFF0000u) | static_cast<std::uint16_t>(regs_.eip + displacement);
        return;
    }
    if (opcode == 0xE9 || opcode == 0xE8) {
        const std::int32_t displacement = operand32 ? static_cast<std::int32_t>(fetch32()) : static_cast<std::int16_t>(fetch16());
        if (opcode == 0xE8) push(regs_.eip & mask_for(operand32 ? 32u : 16u), operand32);
        if (operand32) regs_.eip += static_cast<std::uint32_t>(displacement);
        else regs_.eip = (regs_.eip & 0xFFFF0000u) | static_cast<std::uint16_t>(regs_.eip + displacement);
        return;
    }
    if (opcode == 0xC3) {
        const auto target = pop(operand32);
        if (operand32) regs_.eip = target;
        else regs_.eip = (regs_.eip & 0xFFFF0000u) | (target & 0xFFFFu);
        return;
    }

    // MOV between register and register/memory, including byte operands.
    if (opcode >= 0x88 && opcode <= 0x8B) {
        Operand rm{}; std::uint8_t reg = 0;
        if (!decode_modrm(rm, reg, address32, has_segment_override, segment_override)) return;
        const bool byte_width = (opcode & 1u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        const bool direction = (opcode & 2u) != 0;
        Operand reg_operand{true, reg, 0};
        if (direction) write_operand(reg_operand, width, read_operand(rm, width));
        else write_operand(rm, width, read_operand(reg_operand, width));
        return;
    }

    if (opcode == 0xC6 || opcode == 0xC7) {
        Operand rm{}; std::uint8_t group = 0;
        if (!decode_modrm(rm, group, address32, has_segment_override, segment_override)) return;
        if (group != 0) { last_error_ = "unsupported MOV immediate ModRM group"; halted_ = true; return; }
        const unsigned width = opcode == 0xC6 ? 8u : (operand32 ? 32u : 16u);
        const auto value = width == 8 ? fetch8() : (width == 16 ? fetch16() : fetch32());
        write_operand(rm, width, value);
        return;
    }

    // ADD, OR, AND, SUB, XOR, CMP with register/memory operands.
    const auto alu_base = static_cast<std::uint8_t>(opcode & 0xF8u);
    if ((opcode & 0x04u) == 0 &&
        (alu_base == 0x00 || alu_base == 0x08 || alu_base == 0x20 ||
         alu_base == 0x28 || alu_base == 0x30 || alu_base == 0x38)) {
        Operand rm{}; std::uint8_t reg = 0;
        if (!decode_modrm(rm, reg, address32, has_segment_override, segment_override)) return;
        const bool byte_width = (opcode & 1u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        const bool direction = (opcode & 2u) != 0;
        Operand reg_operand{true, reg, 0};
        const auto rm_value = read_operand(rm, width);
        const auto reg_value = read_operand(reg_operand, width);
        const auto lhs = direction ? reg_value : rm_value;
        const auto rhs = direction ? rm_value : reg_value;
        const auto operation = static_cast<std::uint8_t>(alu_base >> 3);
        const auto result = do_alu(operation, lhs, rhs, width);
        if (operation != 7) {
            if (direction) write_operand(reg_operand, width, result);
            else write_operand(rm, width, result);
        }
        return;
    }

    // Accumulator-immediate forms: ADD/OR/AND/SUB/XOR/CMP AL, AX, or EAX.
    const auto accumulator_base = static_cast<std::uint8_t>(opcode & 0xF8u);
    if ((accumulator_base == 0x00 || accumulator_base == 0x08 || accumulator_base == 0x20 ||
         accumulator_base == 0x28 || accumulator_base == 0x30 || accumulator_base == 0x38) &&
        (opcode & 0x06u) == 0x04u) {
        const bool byte_width = (opcode & 1u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        const auto immediate = width == 8 ? fetch8() : (width == 16 ? fetch16() : fetch32());
        Operand accumulator{true, 0, 0};
        const auto lhs = read_operand(accumulator, width);
        const auto result = do_alu(static_cast<std::uint8_t>(accumulator_base >> 3), lhs, immediate, width);
        if ((accumulator_base >> 3) != 7) write_operand(accumulator, width, result);
        return;
    }

    // Group 1 immediate arithmetic: 80h, 81h, and sign-extended 83h.
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        Operand rm{}; std::uint8_t group = 0;
        if (!decode_modrm(rm, group, address32, has_segment_override, segment_override)) return;
        const unsigned width = opcode == 0x80 ? 8u : (operand32 ? 32u : 16u);
        std::uint32_t immediate = 0;
        if (opcode == 0x83) immediate = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(fetch8())));
        else if (width == 8) immediate = fetch8();
        else if (width == 16) immediate = fetch16();
        else immediate = fetch32();
        const auto lhs = read_operand(rm, width);
        const auto result = do_alu(group, lhs, immediate, width);
        if (group != 7) write_operand(rm, width, result);
        return;
    }

    // Moffs accumulator transfers (A0-A3), using a 16-bit real-mode offset.
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        if (address32) { last_error_ = "32-bit moffs addressing is not implemented yet"; halted_ = true; return; }
        const auto offset = fetch16();
        const auto segment = has_segment_override ? segment_override : regs_.ds;
        const auto address = physical(segment, offset);
        const bool byte_width = (opcode & 1u) == 0;
        const bool load = (opcode & 2u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        Operand accumulator{true, 0, 0};
        if (load) write_operand(accumulator, width, width == 8 ? read8(address) : (width == 16 ? read16(address) : read32(address)));
        else {
            const auto value = read_operand(accumulator, width);
            if (width == 8) write8(address, static_cast<std::uint8_t>(value));
            else if (width == 16) write16(address, static_cast<std::uint16_t>(value));
            else write32(address, value);
        }
        return;
    }

    // Short/near immediate arithmetic against AL/AX/EAX is also covered above.
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
