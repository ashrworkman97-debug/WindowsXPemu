#include "xprevival/cpu.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using xprevival::Cpu;
    Cpu cpu;
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

    Cpu memory_test;
    memory_test.write16(0x100, 0xBEEF);
    assert(memory_test.read16(0x100) == 0xBEEF);

    std::cout << "CPU core tests passed.\n";
    return 0;
}
