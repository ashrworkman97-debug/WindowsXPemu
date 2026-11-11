#include "xprevival/cpu.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
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
        std::cout << "XP Revival CPU prototype 0.5.0\n"
                  << "Usage: xprevival [--ram-mib N] <raw-x86-binary>\n"
                  << "Default RAM: 128 MiB (386 registers; experimental protected mode; paging off).\n";
        return 0;
    }

    std::ifstream input(program_path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open: " << program_path << '\n';
        return 1;
    }

    std::vector<std::uint8_t> program;
    for (char byte{}; input.get(byte);) {
        program.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
    if (input.bad()) {
        std::cerr << "Error while reading: " << program_path << '\n';
        return 1;
    }

    try {
        xprevival::Cpu cpu(ram_mib * 1024u * 1024u);
        cpu.load(program, 0x7C00);
        auto& regs = cpu.registers();
        regs.cs = 0x0000;
        regs.eip = 0x7C00;
        regs.ss = 0x0000;
        regs.esp = 0x7C00;

        cpu.run(100000);
        const auto& r = cpu.registers();
        std::cout << "RAM=" << cpu.ram_size() / (1024u * 1024u) << " MiB "
                  << "EAX=" << std::hex << std::setw(8) << std::setfill('0') << r.eax
                  << " EBX=" << std::setw(8) << r.ebx
                  << " ECX=" << std::setw(8) << r.ecx
                  << " EDX=" << std::setw(8) << r.edx
                  << " CS:EIP=" << std::setw(4) << r.cs << ':' << std::setw(8) << r.eip
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
