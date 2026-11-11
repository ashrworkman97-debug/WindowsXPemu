#include "xprevival/pc.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool parse_size(std::string_view value, std::size_t& output) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

std::vector<std::uint8_t> read_binary(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open: " + path);

    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("could not determine file size: " + path);
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error("file is too large for this host: " + path);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw std::runtime_error("error while reading: " + path);
    }
    return bytes;
}

void write_binary(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not open disk image for writing: " + path);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("error while writing disk image: " + path);
    }
}

void print_help() {
    std::cout
        << "XP Revival PC emulator 0.10.0 (Windows x64)\n"
        << "Usage:\n"
        << "  xprevival.exe --demo [--cdrom <image.iso>] [--ram-mib N] [--steps N]\n"
        << "  xprevival.exe --disk <raw-disk.img> [--cdrom <image.iso>] [--ram-mib N] [--steps N] [--write-disk]\n"
        << "  xprevival.exe [--ram-mib N] <raw-x86-binary>\n\n"
        << "The disk path loads sector 0 at 0000:7C00. --cdrom attaches an ISO as primary-slave ATAPI.\n"
        << "Disk writes stay in memory unless --write-disk is supplied.\n"
        << "Default RAM: 128 MiB; default instruction budget: 1,000,000.\n"
        << "This is an early PC prototype, not yet a Windows XP or ISO boot emulator.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::size_t ram_mib = xprevival::Cpu::default_ram_size / (1024u * 1024u);
    std::size_t instruction_limit = 1000000;
    std::string disk_path;
    std::string cdrom_path;
    std::string program_path;
    bool demo = false;
    bool write_disk = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        if (arg == "--ram-mib" && i + 1 < argc) {
            if (!parse_size(argv[++i], ram_mib) || ram_mib < 1 || ram_mib > 2048) {
                std::cerr << "RAM must be between 1 and 2048 MiB.\n";
                return 1;
            }
        } else if (arg == "--steps" && i + 1 < argc) {
            if (!parse_size(argv[++i], instruction_limit) || instruction_limit == 0 ||
                instruction_limit > 100000000u) {
                std::cerr << "Instruction budget must be between 1 and 100000000.\n";
                return 1;
            }
        } else if (arg == "--disk" && i + 1 < argc) {
            if (!disk_path.empty()) {
                std::cerr << "Only one disk image can be attached.\n";
                return 1;
            }
            disk_path = argv[++i];
        } else if (arg == "--cdrom" && i + 1 < argc) {
            if (!cdrom_path.empty()) {
                std::cerr << "Only one CD-ROM image can be attached.\n";
                return 1;
            }
            cdrom_path = argv[++i];
        } else if (arg == "--demo") {
            demo = true;
        } else if (arg == "--write-disk") {
            write_disk = true;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown option: " << arg << '\n';
            return 1;
        } else if (program_path.empty()) {
            program_path = argv[i];
        } else {
            std::cerr << "Unexpected argument: " << argv[i] << '\n';
            return 1;
        }
    }

    const bool disk_mode = demo || !disk_path.empty();
    if ((demo && !disk_path.empty()) || (disk_mode && !program_path.empty()) ||
        (!cdrom_path.empty() && !disk_mode) || (write_disk && (demo || disk_path.empty()))) {
        std::cerr << "Choose either --demo, --disk <image>, or a raw program; --write-disk only applies to --disk.\n";
        return 1;
    }

    try {
        const auto ram_bytes = ram_mib * 1024u * 1024u;
        if (disk_mode) {
            auto image = demo ? xprevival::make_demo_disk_image() : read_binary(disk_path);
            xprevival::Pc pc = cdrom_path.empty()
                ? xprevival::Pc(std::move(image), ram_bytes)
                : xprevival::Pc(std::move(image), read_binary(cdrom_path), ram_bytes);
            pc.boot();
            const auto executed = pc.run(instruction_limit);

            std::cout << "XP Revival original PC boot\n"
                      << "RAM: " << ram_mib << " MiB\n"
                      << "Primary-master IDE: " << pc.disk().image().size() / 512u << " sectors\n"
                      << "Instructions executed: " << executed << '\n'
                      << "Primary-slave ATAPI CD-ROM: " << (pc.cdrom().has_media()
                            ? std::to_string(pc.cdrom().image().size() / xprevival::AtapiCdromDevice::sector_bytes) + " 2048-byte sectors"
                            : "not mounted") << '\n'
                      << "VGA text memory (B800:0000):\n"
                      << pc.text_screen();

            if (!pc.cpu().last_error().empty()) {
                std::cerr << "CPU stopped: " << pc.cpu().last_error() << '\n';
                return 2;
            }
            if (!pc.cpu().halted()) {
                std::cout << "Instruction budget reached before HLT.\n";
            } else {
                std::cout << "Guest CPU halted normally.\n";
            }

            if (write_disk) {
                write_binary(disk_path, pc.disk().image());
                std::cout << "Saved guest disk changes to " << disk_path << ".\n";
            } else if (!demo) {
                std::cout << "Disk changes are memory-only; use --write-disk to persist them.\n";
            }
            return 0;
        }

        if (program_path.empty()) {
            print_help();
            return 0;
        }

        const auto program = read_binary(program_path);
        xprevival::Cpu cpu(ram_bytes);
        cpu.load(program, 0x7C00);
        auto& regs = cpu.registers();
        regs.cs = 0x0000;
        regs.eip = 0x7C00;
        regs.ss = 0x0000;
        regs.esp = 0x7C00;

        cpu.run(instruction_limit);
        const auto& result = cpu.registers();
        std::cout << "XP Revival CPU test harness\n"
                  << "RAM=" << cpu.ram_size() / (1024u * 1024u) << " MiB "
                  << "EAX=" << std::hex << std::setw(8) << std::setfill('0') << result.eax
                  << " EBX=" << std::setw(8) << result.ebx
                  << " ECX=" << std::setw(8) << result.ecx
                  << " EDX=" << std::setw(8) << result.edx
                  << " CS:EIP=" << std::setw(4) << result.cs << ':' << std::setw(8) << result.eip
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
