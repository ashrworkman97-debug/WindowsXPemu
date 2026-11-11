#include "xprevival/cpu.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cout << "XP Revival CPU prototype 0.1.0\n"
                  << "Usage: xprevival <raw-real-mode-binary>\n";
        return 0;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Could not open: " << argv[1] << '\n';
        return 1;
    }
    std::vector<std::uint8_t> program(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());

    xprevival::Cpu cpu;
    // Test binaries are loaded at 0000:7C00, the conventional boot-sector
    // address. This is a CPU-core harness, not a BIOS/PC boot implementation.
    cpu.load(program, 0x7C00);
    auto& regs = const_cast<xprevival::Cpu::Registers&>(cpu.registers());
    regs.cs = 0x0000;
    regs.ip = 0x7C00;

    cpu.run(100000);
    const auto& r = cpu.registers();
    std::cout << "AX=" << std::hex << std::setw(4) << std::setfill('0') << r.ax
              << " BX=" << std::setw(4) << r.bx
              << " CX=" << std::setw(4) << r.cx
              << " DX=" << std::setw(4) << r.dx
              << " CS:IP=" << std::setw(4) << r.cs << ':' << std::setw(4) << r.ip
              << std::dec << '\n';
    if (!cpu.last_error().empty()) {
        std::cerr << cpu.last_error() << '\n';
        return 2;
    }
    if (!cpu.halted()) std::cout << "Instruction limit reached.\n";
    return 0;
}
