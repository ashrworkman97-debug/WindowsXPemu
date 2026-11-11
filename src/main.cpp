#include "xprevival/cpu.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::size_t ram_mib = xprevival::Cpu::default_ram_size / (1024u * 1024u);
    const char* program_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--ram-mib" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), ram_mib);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || ram_mib < 1 || ram_mib > 2048) {
                std::cerr << "RAM must be between 1 and 2048 MiB.\n";
                return 1;
            }
        } else if (!program_path) {
            program_path = argv[i];
        } else {
            std::cerr << "Unexpected argument: " << argv[i] << '\n';
            return 1;
        }
    }

    if (!program_path) {
        std::cout << "XP Revival CPU prototype 0.2.0\n"
                  << "Usage: xprevival [--ram-mib N] <raw-real-mode-binary>\n"
                  << "Default RAM: 128 MiB (CPU currently executes real-mode code only).\n";
        return 0;
    }

    std::ifstream input(program_path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open: " << program_path << '\n';
        return 1;
    }
    std::vector<std::uint8_t> program(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());

    try {
        xprevival::Cpu cpu(ram_mib * 1024u * 1024u);
        // Test binaries are loaded at 0000:7C00. This harness does not yet
        // implement BIOS services or boot a disk/ISO.
        cpu.load(program, 0x7C00);
        auto& regs = cpu.registers();
        regs.cs = 0x0000;
        regs.ip = 0x7C00;

        cpu.run(100000);
        const auto& r = cpu.registers();
        std::cout << "RAM=" << cpu.ram_size() / (1024u * 1024u) << " MiB "
                  << "AX=" << std::hex << std::setw(4) << std::setfill('0') << r.ax
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
    } catch (const std::exception& error) {
        std::cerr << "Emulator error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
