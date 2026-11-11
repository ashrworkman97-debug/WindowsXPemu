#include "xprevival/cpu.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    using xprevival::Cpu;
    Cpu cpu(4u * 1024u * 1024u);
    assert(cpu.ram_size() == 4u * 1024u * 1024u);
    assert(cpu.registers().cs == 0xF000);
    assert(cpu.registers().ip == 0xFFF0);

    // MOV AX,1234h ; MOV BX,5678h ; NOP ; HLT
    constexpr std::array<std::uint8_t, 8> program{
        0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56, 0x90, 0xF4
    };
    cpu.load(program, 0x7C00);
    auto& regs = cpu.registers();
    regs.cs = 0;
    regs.ip = 0x7C00;
    cpu.run(16);

    assert(cpu.registers().ax == 0x1234);
    assert(cpu.registers().bx == 0x5678);
    assert(cpu.halted());
    assert(cpu.last_error().empty());

    // The RAM backing store reaches beyond the original 1 MiB limit.
    cpu.write16(2u * 1024u * 1024u, 0xBEEF);
    assert(cpu.read16(2u * 1024u * 1024u) == 0xBEEF);

    bool rejected_small_ram = false;
    try { Cpu too_small(512u * 1024u); }
    catch (const std::invalid_argument&) { rejected_small_ram = true; }
    assert(rejected_small_ram);

    bool rejected_out_of_range = false;
    try { cpu.read8(4u * 1024u * 1024u); }
    catch (const std::out_of_range&) { rejected_out_of_range = true; }
    assert(rejected_out_of_range);

    std::cout << "CPU core tests passed.\n";
    return 0;
}
