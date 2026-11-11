#include "xprevival/cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "CHECK failed on line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

} // namespace

int main() {
    using xprevival::Cpu;
    Cpu cpu(4u * 1024u * 1024u);
    CHECK(cpu.ram_size() == 4u * 1024u * 1024u);
    CHECK(cpu.registers().cs == 0xF000);
    CHECK(cpu.registers().ip == 0xFFF0);

    // MOV AX,1234h ; MOV BX,5678h ; NOP ; HLT
    constexpr std::array<std::uint8_t, 8> program{
        0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56, 0x90, 0xF4
    };
    cpu.load(program, 0x7C00);
    auto& regs = cpu.registers();
    regs.cs = 0;
    regs.ip = 0x7C00;
    cpu.run(16);

    CHECK(cpu.registers().ax == 0x1234);
    CHECK(cpu.registers().bx == 0x5678);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // The RAM backing store reaches beyond the original 1 MiB limit.
    cpu.write16(2u * 1024u * 1024u, 0xBEEF);
    CHECK(cpu.read16(2u * 1024u * 1024u) == 0xBEEF);

    bool rejected_small_ram = false;
    try { Cpu too_small(512u * 1024u); }
    catch (const std::invalid_argument&) { rejected_small_ram = true; }
    CHECK(rejected_small_ram);

    bool rejected_out_of_range = false;
    try { (void)cpu.read8(4u * 1024u * 1024u); }
    catch (const std::out_of_range&) { rejected_out_of_range = true; }
    CHECK(rejected_out_of_range);

    bool rejected_oversized_load = false;
    try {
        constexpr std::array<std::uint8_t, 2> too_large{0x90, 0xF4};
        cpu.load(too_large, static_cast<std::uint32_t>(cpu.ram_size() - 1));
    } catch (const std::out_of_range&) {
        rejected_oversized_load = true;
    }
    CHECK(rejected_oversized_load);

    std::cout << "CPU core tests passed.\n";
    return EXIT_SUCCESS;
}
