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

void Cpu::set_io_callbacks(IoReadCallback read_callback, IoWriteCallback write_callback) {
    io_read_callback_ = read_callback;
    io_write_callback_ = write_callback;
}

std::uint32_t Cpu::read_port(std::uint16_t port, unsigned width) {
    const auto mask = mask_for(width);
    if (!io_read_callback_) return mask;
    return io_read_callback_(port, width) & mask;
}
void Cpu::write_port(std::uint16_t port, unsigned width, std::uint32_t value) {
    if (io_write_callback_) io_write_callback_(port, width, value & mask_for(width));
}

void Cpu::reset() {
    std::fill(memory_.begin(), memory_.end(), std::uint8_t{0});
    regs_ = {};
    regs_.eflags = 0x00000002;
    segments_[0] = {}; segments_[1] = {}; segments_[2] = {};
    segments_[3] = {}; segments_[4] = {}; segments_[5] = {};
    gdtr_base_ = 0;
    gdtr_limit_ = 0;
    last_page_fault_error_ = 0;
    tsc_ = 0;
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

std::uint32_t Cpu::physical(SegmentIndex segment, std::uint32_t offset) const noexcept {
    if ((regs_.cr0 & 1u) == 0) {
        std::uint16_t selector = 0;
        switch (segment) {
            case SegmentIndex::ES: selector = regs_.es; break;
            case SegmentIndex::CS: selector = regs_.cs; break;
            case SegmentIndex::SS: selector = regs_.ss; break;
            case SegmentIndex::DS: selector = regs_.ds; break;
            case SegmentIndex::FS: selector = regs_.fs; break;
            case SegmentIndex::GS: selector = regs_.gs; break;
        }
        return (static_cast<std::uint32_t>(selector) << 4) + offset;
    }
    return segments_[static_cast<std::size_t>(segment)].base + offset;
}

bool Cpu::load_segment(SegmentIndex segment, std::uint16_t selector) {
    auto& cache = segments_[static_cast<std::size_t>(segment)];
    const auto set_selector = [this, segment, selector]() {
        switch (segment) {
            case SegmentIndex::ES: regs_.es = selector; break;
            case SegmentIndex::CS: regs_.cs = selector; break;
            case SegmentIndex::SS: regs_.ss = selector; break;
            case SegmentIndex::DS: regs_.ds = selector; break;
            case SegmentIndex::FS: regs_.fs = selector; break;
            case SegmentIndex::GS: regs_.gs = selector; break;
        }
    };

    if ((regs_.cr0 & 1u) == 0) {
        cache.base = static_cast<std::uint32_t>(selector) << 4;
        cache.limit = 0xFFFF;
        cache.default32 = false;
        cache.valid = true;
        set_selector();
        return true;
    }

    const bool nullable = segment == SegmentIndex::ES || segment == SegmentIndex::DS ||
                          segment == SegmentIndex::FS || segment == SegmentIndex::GS;
    if ((selector & 0xFFF8u) == 0) {
        if (!nullable) {
            last_error_ = "null selector is not valid for CS or SS";
            halted_ = true;
            return false;
        }
        cache = {0, 0, false, false};
        set_selector();
        return true;
    }
    if ((selector & 0x4u) != 0) {
        last_error_ = "LDT selectors are not implemented; use a GDT selector";
        halted_ = true;
        return false;
    }

    const auto descriptor_offset = static_cast<std::uint32_t>(selector & 0xFFF8u);
    if (descriptor_offset + 7u > gdtr_limit_) {
        last_error_ = "segment selector lies outside the current GDT limit";
        halted_ = true;
        return false;
    }
    const auto address = gdtr_base_ + descriptor_offset;
    const auto limit_low = read_linear16(address);
    const auto base_low = read_linear16(address + 2);
    const auto base_mid = read_linear8(address + 4);
    const auto access = read_linear8(address + 5);
    const auto flags_limit = read_linear8(address + 6);
    const auto base_high = read_linear8(address + 7);

    if ((access & 0x80u) == 0 || (access & 0x10u) == 0) {
        last_error_ = "GDT descriptor is not a present code/data segment";
        halted_ = true;
        return false;
    }
    const bool code = (access & 0x08u) != 0;
    const bool readable_or_writable = (access & 0x02u) != 0;
    if (segment == SegmentIndex::CS && !code) {
        last_error_ = "CS requires an executable code descriptor";
        halted_ = true;
        return false;
    }
    if (segment == SegmentIndex::SS && (code || !readable_or_writable)) {
        last_error_ = "SS requires a writable data descriptor";
        halted_ = true;
        return false;
    }
    if (segment != SegmentIndex::CS && segment != SegmentIndex::SS && code && !readable_or_writable) {
        last_error_ = "data segment cannot load an execute-only code descriptor";
        halted_ = true;
        return false;
    }

    std::uint32_t limit = static_cast<std::uint32_t>(limit_low) |
                          (static_cast<std::uint32_t>(flags_limit & 0x0Fu) << 16);
    if ((flags_limit & 0x80u) != 0) limit = (limit << 12) | 0xFFFu;
    const auto base = static_cast<std::uint32_t>(base_low) |
                      (static_cast<std::uint32_t>(base_mid) << 16) |
                      (static_cast<std::uint32_t>(base_high) << 24);
    cache.base = base;
    cache.limit = limit;
    cache.default32 = (flags_limit & 0x40u) != 0;
    cache.valid = true;
    set_selector();
    return true;
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

[[noreturn]] void Cpu::record_page_fault(std::uint32_t linear, bool write, bool present, bool reserved) {
    regs_.cr2 = linear;
    last_page_fault_error_ = (present ? 1u : 0u) | (write ? 2u : 0u) | (reserved ? 8u : 0u);
    last_error_ = "page fault at linear address " + std::to_string(linear) +
                  " (error code " + std::to_string(last_page_fault_error_) + ")";
    halted_ = true;
    throw PageFault{};
}

std::uint32_t Cpu::translate_linear(std::uint32_t linear, bool write) {
    constexpr std::uint32_t PagePresent = 1u << 0;
    constexpr std::uint32_t PageWritable = 1u << 1;
    constexpr std::uint32_t PageAccessed = 1u << 5;
    constexpr std::uint32_t PageDirty = 1u << 6;
    constexpr std::uint32_t PageSize = 1u << 7;
    constexpr std::uint32_t PageWriteProtect = 1u << 16;

    if ((regs_.cr0 & 0x80000000u) == 0) return linear;
    if ((regs_.cr0 & 1u) == 0) {
        last_error_ = "CR0.PG is set while CR0.PE is clear";
        halted_ = true;
        throw PageFault{};
    }

    // 386 two-level paging: 10-bit directory, 10-bit table, 12-bit offset.
    // Page-table walks use physical RAM; guest fetches and data use linear addresses.
    const auto directory_address = (regs_.cr3 & 0xFFFFF000u) + ((linear >> 22) & 0x3FFu) * 4u;
    if (static_cast<std::uint64_t>(directory_address) + 4u > memory_.size())
        record_page_fault(linear, write, false);
    auto directory_entry = read32(directory_address);
    if ((directory_entry & PagePresent) == 0) record_page_fault(linear, write, false);
    if ((directory_entry & PageSize) != 0) record_page_fault(linear, write, true, true);

    // Set the PDE accessed bit even when the following PTE lookup faults.
    if ((directory_entry & PageAccessed) == 0) {
        directory_entry |= PageAccessed;
        write32(directory_address, directory_entry);
    }

    const auto table_address = (directory_entry & 0xFFFFF000u) + ((linear >> 12) & 0x3FFu) * 4u;
    if (static_cast<std::uint64_t>(table_address) + 4u > memory_.size())
        record_page_fault(linear, write, false);
    auto table_entry = read32(table_address);
    if ((table_entry & PagePresent) == 0) record_page_fault(linear, write, false);

    // The current core models supervisor/CPL 0. CR0.WP makes supervisor
    // writes honor read-only entries; user/supervisor privilege checks come later.
    const bool writable = (directory_entry & PageWritable) != 0 &&
                          (table_entry & PageWritable) != 0;
    if (write && !writable && (regs_.cr0 & PageWriteProtect) != 0)
        record_page_fault(linear, true, true);

    table_entry |= PageAccessed;
    if (write) table_entry |= PageDirty;
    write32(table_address, table_entry);

    const auto physical_address = (table_entry & 0xFFFFF000u) | (linear & 0xFFFu);
    if (physical_address >= memory_.size())
        throw std::out_of_range("translated physical address is outside emulated RAM");
    return physical_address;
}

std::uint8_t Cpu::read_linear8(std::uint32_t address) {
    return read8(translate_linear(address, false));
}
std::uint16_t Cpu::read_linear16(std::uint32_t address) {
    return static_cast<std::uint16_t>(read_linear8(address) |
        (static_cast<std::uint16_t>(read_linear8(address + 1u)) << 8));
}
std::uint32_t Cpu::read_linear32(std::uint32_t address) {
    return static_cast<std::uint32_t>(read_linear16(address)) |
        (static_cast<std::uint32_t>(read_linear16(address + 2u)) << 16);
}
void Cpu::write_linear8(std::uint32_t address, std::uint8_t value) {
    write8(translate_linear(address, true), value);
}
void Cpu::write_linear16(std::uint32_t address, std::uint16_t value) {
    write_linear8(address, static_cast<std::uint8_t>(value & 0xFFu));
    write_linear8(address + 1u, static_cast<std::uint8_t>(value >> 8));
}
void Cpu::write_linear32(std::uint32_t address, std::uint32_t value) {
    write_linear16(address, static_cast<std::uint16_t>(value & 0xFFFFu));
    write_linear16(address + 2u, static_cast<std::uint16_t>(value >> 16));
}

std::uint8_t Cpu::fetch8() {
    const bool code32 = segments_[static_cast<std::size_t>(SegmentIndex::CS)].default32;
    const auto offset = code32 ? regs_.eip : (regs_.eip & 0xFFFFu);
    const auto value = read_linear8(physical(SegmentIndex::CS, offset));
    if (code32) ++regs_.eip;
    else regs_.eip = (regs_.eip & 0xFFFF0000u) | ((regs_.eip + 1u) & 0xFFFFu);
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
                       bool has_segment_override, SegmentIndex segment_override) {
    const auto modrm = fetch8();
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    reg = static_cast<std::uint8_t>((modrm >> 3) & 7u);
    const auto rmi = static_cast<std::uint8_t>(modrm & 7u);
    if (mod == 3) {
        rm = {true, rmi, 0};
        return true;
    }
    if (address32) {
        std::uint32_t base = 0;
        std::uint32_t index_value = 0;
        bool uses_stack_segment = false;
        bool has_base = true;
        if (rmi == 4) {
            const auto sib = fetch8();
            const auto scale = static_cast<std::uint8_t>(sib >> 6);
            const auto index = static_cast<std::uint8_t>((sib >> 3) & 7u);
            const auto base_index = static_cast<std::uint8_t>(sib & 7u);
            if (index != 4) index_value = read_general(index, true) << scale;
            if (mod == 0 && base_index == 5) has_base = false;
            else {
                base = read_general(base_index, true);
                uses_stack_segment = base_index == 4 || base_index == 5;
            }
        } else if (mod == 0 && rmi == 5) {
            has_base = false;
        } else {
            base = read_general(rmi, true);
            uses_stack_segment = rmi == 4 || rmi == 5;
        }

        std::int32_t displacement = 0;
        if (!has_base) displacement = static_cast<std::int32_t>(fetch32());
        else if (mod == 1) displacement = static_cast<std::int8_t>(fetch8());
        else if (mod == 2) displacement = static_cast<std::int32_t>(fetch32());

        const auto offset = base + index_value + static_cast<std::uint32_t>(displacement);
        const auto segment = has_segment_override ? segment_override :
            (uses_stack_segment ? SegmentIndex::SS : SegmentIndex::DS);
        rm = {false, 0, physical(segment, offset)};
        return true;
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
    const auto segment = has_segment_override ? segment_override : (uses_bp ? SegmentIndex::SS : SegmentIndex::DS);
    rm = {false, 0, physical(segment, offset)};
    return true;
}

std::uint32_t Cpu::read_operand(const Operand& operand, unsigned width) {
    if (width == 8) return operand.is_register ? read_byte_register(operand.reg) : read_linear8(operand.address);
    if (width == 16) return operand.is_register ? read_general(operand.reg, false) : read_linear16(operand.address);
    return operand.is_register ? read_general(operand.reg, true) : read_linear32(operand.address);
}
void Cpu::write_operand(const Operand& operand, unsigned width, std::uint32_t value) {
    if (width == 8) {
        if (operand.is_register) write_byte_register(operand.reg, static_cast<std::uint8_t>(value));
        else write_linear8(operand.address, static_cast<std::uint8_t>(value));
    } else if (width == 16) {
        if (operand.is_register) write_general(operand.reg, false, value);
        else write_linear16(operand.address, static_cast<std::uint16_t>(value));
    } else {
        if (operand.is_register) write_general(operand.reg, true, value);
        else write_linear32(operand.address, value);
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
    const bool stack32 = segments_[static_cast<std::size_t>(SegmentIndex::SS)].default32;
    std::uint32_t pointer = stack32 ? regs_.esp : (regs_.esp & 0xFFFFu);
    pointer -= operand32 ? 4u : 2u;
    if (stack32) regs_.esp = pointer;
    else regs_.esp = (regs_.esp & 0xFFFF0000u) | (pointer & 0xFFFFu);
    const auto address = physical(SegmentIndex::SS, pointer);
    if (operand32) write_linear32(address, value);
    else write_linear16(address, static_cast<std::uint16_t>(value));
}
std::uint32_t Cpu::pop(bool operand32) {
    const bool stack32 = segments_[static_cast<std::size_t>(SegmentIndex::SS)].default32;
    const std::uint32_t pointer = stack32 ? regs_.esp : (regs_.esp & 0xFFFFu);
    const auto address = physical(SegmentIndex::SS, pointer);
    const auto value = operand32 ? read_linear32(address) : read_linear16(address);
    const auto next = pointer + (operand32 ? 4u : 2u);
    if (stack32) regs_.esp = next;
    else regs_.esp = (regs_.esp & 0xFFFF0000u) | (next & 0xFFFFu);
    return value;
}

void Cpu::step() {
    if (halted_) return;
    try {
    const auto instruction_start = regs_.eip;
    const bool default32 = segments_[static_cast<std::size_t>(SegmentIndex::CS)].default32;
    bool operand32 = default32, address32 = default32, has_segment_override = false;
    std::uint8_t repeat_prefix = 0;
    SegmentIndex segment_override = SegmentIndex::DS;
    std::uint8_t opcode = fetch8();
    std::size_t prefix_count = 0;
    while (prefix_count < 15) {
        bool consumed = true;
        switch (opcode) {
            case 0x66: operand32 = !default32; break;
            case 0x67: address32 = !default32; break;
            case 0x26: segment_override = SegmentIndex::ES; has_segment_override = true; break;
            case 0x2E: segment_override = SegmentIndex::CS; has_segment_override = true; break;
            case 0x36: segment_override = SegmentIndex::SS; has_segment_override = true; break;
            case 0x3E: segment_override = SegmentIndex::DS; has_segment_override = true; break;
            case 0x64: segment_override = SegmentIndex::FS; has_segment_override = true; break;
            case 0x65: segment_override = SegmentIndex::GS; has_segment_override = true; break;
            case 0xF2: repeat_prefix = 0xF2; break;
            case 0xF3: repeat_prefix = 0xF3; break;
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
    if (opcode == 0xFA) { regs_.eflags &= ~Interrupt; return; } // CLI
    if (opcode == 0xFB) { regs_.eflags |= Interrupt; return; }  // STI
    if (opcode == 0xFC) { regs_.eflags &= ~Direction; return; } // CLD
    if (opcode == 0xFD) { regs_.eflags |= Direction; return; }  // STD

    if (opcode == 0x0F) {
        const auto second = fetch8();
        if (second == 0x20 || second == 0x22) {
            const auto modrm = fetch8();
            if ((modrm & 0xC0u) != 0xC0u) {
                last_error_ = "MOV control-register form requires a general-purpose register";
                halted_ = true;
                return;
            }
            const auto control = static_cast<std::uint8_t>((modrm >> 3) & 7u);
            const auto general = static_cast<std::uint8_t>(modrm & 7u);
            if (second == 0x20) {
                std::uint32_t value = 0;
                switch (control) {
                    case 0: value = regs_.cr0; break;
                    case 2: value = regs_.cr2; break;
                    case 3: value = regs_.cr3; break;
                    case 4: value = regs_.cr4; break;
                    default: last_error_ = "unsupported control register read"; halted_ = true; return;
                }
                write_general(general, true, value);
            } else {
                const auto value = read_general(general, true);
                switch (control) {
                    case 0:
                        if ((value & 0x80000000u) != 0 && (value & 1u) == 0) {
                            last_error_ = "CR0.PG cannot be enabled while CR0.PE is clear";
                            halted_ = true;
                            return;
                        }
                        regs_.cr0 = value;
                        break;
                    case 2: regs_.cr2 = value; break;
                    case 3: regs_.cr3 = value; break;
                    case 4: regs_.cr4 = value; break;
                    default: last_error_ = "unsupported control register write"; halted_ = true; return;
                }
            }
            return;
        }

        if (second == 0xA2) { // CPUID: deterministic Intel Pentium III baseline.
            const auto leaf = regs_.eax;
            if (leaf == 0) {
                regs_.eax = 1;
                regs_.ebx = 0x756E6547u; // Genu
                regs_.edx = 0x49656E69u; // ineI
                regs_.ecx = 0x6C65746Eu; // ntel
            } else if (leaf == 1) {
                regs_.eax = 0x00000673u; // family 6, model 7, stepping 3
                regs_.ebx = 0;
                regs_.ecx = 0;
                regs_.edx = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) |
                            (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                            (1u << 8) | (1u << 9) | (1u << 11) | (1u << 12) |
                            (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) |
                            (1u << 17) | (1u << 23) | (1u << 24) | (1u << 25);
            } else {
                regs_.eax = regs_.ebx = regs_.ecx = regs_.edx = 0;
            }
            return;
        }
        if (second == 0x31) { // Deterministic instruction-count timestamp counter.
            ++tsc_;
            regs_.eax = static_cast<std::uint32_t>(tsc_);
            regs_.edx = static_cast<std::uint32_t>(tsc_ >> 32);
            return;
        }

        if (second == 0x01) {
            Operand rm{}; std::uint8_t group = 0;
            if (!decode_modrm(rm, group, address32, has_segment_override, segment_override)) return;
            if (rm.is_register || group != 2) {
                last_error_ = "only LGDT m16&32 is implemented for 0F 01";
                halted_ = true;
                return;
            }
            gdtr_limit_ = read_linear16(rm.address);
            if (operand32) gdtr_base_ = read_linear32(rm.address + 2);
            else gdtr_base_ = static_cast<std::uint32_t>(read_linear8(rm.address + 2)) |
                              (static_cast<std::uint32_t>(read_linear16(rm.address + 3)) << 8);
            return;
        }

        last_error_ = "unsupported 0F opcode";
        halted_ = true;
        return;
    }


    // IN/OUT: byte, word, and dword transfers using immediate or DX port numbers.
    if ((opcode >= 0xE4 && opcode <= 0xE7) || (opcode >= 0xEC && opcode <= 0xEF)) {
        const bool input = opcode == 0xE4 || opcode == 0xE5 || opcode == 0xEC || opcode == 0xED;
        const bool byte_width = (opcode & 1u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        const std::uint16_t port = opcode <= 0xE7
            ? static_cast<std::uint16_t>(fetch8())
            : static_cast<std::uint16_t>(regs_.edx);
        if (input) {
            const auto value = read_port(port, width);
            if (width == 8) write_byte_register(0, static_cast<std::uint8_t>(value));
            else write_general(0, operand32, value);
        } else {
            const auto value = width == 8 ? read_byte_register(0) : read_general(0, operand32);
            write_port(port, width, value);
        }
        return;
    }

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

    if (opcode == 0x8E || opcode == 0x8C) {
        Operand rm{}; std::uint8_t segment_reg = 0;
        if (!decode_modrm(rm, segment_reg, address32, has_segment_override, segment_override)) return;
        if (segment_reg > 5 || (opcode == 0x8E && segment_reg == 1)) {
            last_error_ = "invalid segment-register MOV encoding";
            halted_ = true;
            return;
        }
        const auto target_segment = static_cast<SegmentIndex>(segment_reg);
        if (opcode == 0x8E) {
            const auto selector = static_cast<std::uint16_t>(read_operand(rm, 16));
            (void)load_segment(target_segment, selector);
        } else {
            std::uint16_t selector = 0;
            switch (target_segment) {
                case SegmentIndex::ES: selector = regs_.es; break;
                case SegmentIndex::CS: selector = regs_.cs; break;
                case SegmentIndex::SS: selector = regs_.ss; break;
                case SegmentIndex::DS: selector = regs_.ds; break;
                case SegmentIndex::FS: selector = regs_.fs; break;
                case SegmentIndex::GS: selector = regs_.gs; break;
            }
            write_operand(rm, 16, selector);
        }
        return;
    }

    if (opcode == 0xEA) {
        const auto target = operand32 ? fetch32() : fetch16();
        const auto selector = fetch16();
        if (!load_segment(SegmentIndex::CS, selector)) return;
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


    // String instructions. REP/REPE/REPNE operate on CX or ECX according to
    // address size; SI/ESI and DI/EDI wrap at the active address width.
    const bool string_operation = opcode == 0xA4 || opcode == 0xA5 ||
        opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAA || opcode == 0xAB ||
        opcode == 0xAC || opcode == 0xAD || opcode == 0xAE || opcode == 0xAF;
    if (string_operation) {
        const bool byte_width = (opcode & 1u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        const std::uint32_t element_size = width / 8u;
        const bool repeated = repeat_prefix != 0;
        const bool compare = opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF;
        const bool uses_source = opcode == 0xA4 || opcode == 0xA5 ||
                                 opcode == 0xA6 || opcode == 0xA7 ||
                                 opcode == 0xAC || opcode == 0xAD;
        const bool uses_destination = opcode == 0xA4 || opcode == 0xA5 ||
                                      opcode == 0xA6 || opcode == 0xA7 ||
                                      opcode == 0xAA || opcode == 0xAB ||
                                      opcode == 0xAE || opcode == 0xAF;
        auto index_value = [address32](std::uint32_t value) {
            return address32 ? value : (value & 0xFFFFu);
        };
        auto store_index = [address32](std::uint32_t& target, std::uint32_t value) {
            if (address32) target = value;
            else target = (target & 0xFFFF0000u) | (value & 0xFFFFu);
        };
        const auto adjust = (regs_.eflags & Direction) != 0
            ? (0u - element_size) : element_size;
        auto count = repeated ? (address32 ? regs_.ecx : (regs_.ecx & 0xFFFFu)) : 1u;
        if (repeated && count == 0) return;

        constexpr std::uint32_t MaxStringIterationsPerStep = 65536u;
        std::uint32_t iterations = 0;
        bool keep_repeating = true;
        while (count != 0 && iterations < MaxStringIterationsPerStep) {
            const auto source_index = index_value(regs_.esi);
            const auto destination_index = index_value(regs_.edi);
            const auto source_segment = has_segment_override ? segment_override : SegmentIndex::DS;
            const auto source_address = uses_source ? physical(source_segment, source_index) : 0u;
            const auto destination_address = uses_destination
                ? physical(SegmentIndex::ES, destination_index) : 0u;

            auto read_element = [this, width](std::uint32_t address) {
                if (width == 8) return static_cast<std::uint32_t>(read_linear8(address));
                if (width == 16) return static_cast<std::uint32_t>(read_linear16(address));
                return read_linear32(address);
            };
            auto write_element = [this, width](std::uint32_t address, std::uint32_t value) {
                if (width == 8) write_linear8(address, static_cast<std::uint8_t>(value));
                else if (width == 16) write_linear16(address, static_cast<std::uint16_t>(value));
                else write_linear32(address, value);
            };

            if (opcode == 0xA4 || opcode == 0xA5) {
                write_element(destination_address, read_element(source_address));
            } else if (opcode == 0xAA || opcode == 0xAB) {
                write_element(destination_address, read_general(0, width == 32));
            } else if (opcode == 0xAC || opcode == 0xAD) {
                const auto value = read_element(source_address);
                if (width == 8) write_byte_register(0, static_cast<std::uint8_t>(value));
                else write_general(0, width == 32, value);
            } else if (opcode == 0xA6 || opcode == 0xA7) {
                (void)subtract(read_element(source_address), read_element(destination_address), width);
            } else {
                (void)subtract(read_general(0, width == 32), read_element(destination_address), width);
            }

            if (uses_source) store_index(regs_.esi, source_index + adjust);
            if (uses_destination) store_index(regs_.edi, destination_index + adjust);
            ++iterations;

            if (!repeated) break;
            --count;
            if (address32) regs_.ecx = count;
            else regs_.ecx = (regs_.ecx & 0xFFFF0000u) | (count & 0xFFFFu);
            if (count == 0) { keep_repeating = false; break; }
            if (compare) {
                const bool zero = (regs_.eflags & Zero) != 0;
                keep_repeating = repeat_prefix == 0xF3 ? zero : !zero;
                if (!keep_repeating) break;
            }
        }
        if (repeated && count != 0 && keep_repeating && iterations == MaxStringIterationsPerStep)
            regs_.eip = instruction_start;
        return;
    }

    // Moffs accumulator transfers (A0-A3).
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        const auto offset = address32 ? fetch32() : fetch16();
        const auto segment = has_segment_override ? segment_override : SegmentIndex::DS;
        const auto address = physical(segment, offset);
        const bool byte_width = (opcode & 1u) == 0;
        const bool load = (opcode & 2u) == 0;
        const unsigned width = byte_width ? 8u : (operand32 ? 32u : 16u);
        Operand accumulator{true, 0, 0};
        if (load) write_operand(accumulator, width, width == 8 ? read_linear8(address) : (width == 16 ? read_linear16(address) : read_linear32(address)));
        else {
            const auto value = read_operand(accumulator, width);
            if (width == 8) write_linear8(address, static_cast<std::uint8_t>(value));
            else if (width == 16) write_linear16(address, static_cast<std::uint16_t>(value));
            else write_linear32(address, value);
        }
        return;
    }

    // Short/near immediate arithmetic against AL/AX/EAX is also covered above.
    constexpr char digits[] = "0123456789ABCDEF";
    last_error_ = "unsupported opcode 0x";
    last_error_ += digits[(opcode >> 4) & 0xF];
    last_error_ += digits[opcode & 0xF];
    halted_ = true;
    } catch (const PageFault&) {
        return;
    }
}

void Cpu::run(std::size_t instruction_limit) {
    for (std::size_t i = 0; i < instruction_limit && !halted_; ++i) step();
}

} // namespace xprevival
