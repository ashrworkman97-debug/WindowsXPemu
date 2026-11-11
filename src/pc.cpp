#include "xprevival/pc.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace xprevival {
namespace {
constexpr std::size_t boot_sector_bytes = 512;
constexpr std::uint32_t boot_address = 0x7C00;
constexpr std::uint32_t vga_text_address = 0xB8000;
constexpr std::size_t vga_columns = 80;
constexpr std::size_t vga_rows = 25;
constexpr std::size_t vga_bytes = vga_columns * vga_rows * 2;
}

Pc::Pc(std::vector<std::uint8_t> disk_image, std::size_t ram_bytes)
    : cpu_(ram_bytes), ata_(std::move(disk_image)) {
    ata_.connect(cpu_);
    cpu_.set_software_interrupt_callback(
        [this](Cpu& cpu, std::uint8_t vector) { return handle_bios_interrupt(cpu, vector); });
}

void Pc::boot() {
    if (booted_) throw std::logic_error("PC has already been booted");
    const auto& image = ata_.image();
    if (image.size() < boot_sector_bytes)
        throw std::invalid_argument("boot disk is smaller than one sector");
    if (image[510] != 0x55 || image[511] != 0xAA)
        throw std::runtime_error("disk sector 0 has no 55 AA boot signature");

    cpu_.reset();
    cpu_.load(std::span<const std::uint8_t>(image.data(), boot_sector_bytes), boot_address);

    // Conventional x86 BIOS boot handoff: 0000:7C00, stack below the
    // boot-sector area, and DL=80h for the first hard disk.
    vga_cursor_ = 0;
    video_mode_ = 3;
    auto& registers = cpu_.registers();
    registers.cs = 0x0000;
    registers.ds = 0x0000;
    registers.es = 0x0000;
    registers.ss = 0x0000;
    registers.fs = 0x0000;
    registers.gs = 0x0000;
    registers.eip = boot_address;
    registers.esp = boot_address;
    registers.edx = (registers.edx & 0xFFFFFF00u) | 0x80u;
    registers.eflags = 0x00000002u;
    booted_ = true;
}

void Pc::set_interrupt_carry(Cpu& cpu, bool carry) const {
    const auto& registers = cpu.registers();
    const auto stack_offset = (registers.esp + 4u) & 0xFFFFu;
    const auto flags_address = (static_cast<std::uint32_t>(registers.ss) << 4) + stack_offset;
    auto saved_flags = cpu.read16(flags_address);
    if (carry) saved_flags = static_cast<std::uint16_t>(saved_flags | 0x0001u);
    else saved_flags = static_cast<std::uint16_t>(saved_flags & ~0x0001u);
    cpu.write16(flags_address, saved_flags);
}

void Pc::write_teletype_character(Cpu& cpu, std::uint8_t character) {
    constexpr std::uint16_t screen_cells = static_cast<std::uint16_t>(vga_columns * vga_rows);
    const auto row = static_cast<std::uint16_t>(vga_cursor_ / vga_columns);
    const auto column = static_cast<std::uint16_t>(vga_cursor_ % vga_columns);

    if (character == '\r') {
        vga_cursor_ = static_cast<std::uint16_t>(row * vga_columns);
        return;
    }
    if (character == '\n') {
        vga_cursor_ = static_cast<std::uint16_t>((row + 1u) * vga_columns);
    } else if (character == '\b') {
        if (vga_cursor_ != 0) --vga_cursor_;
        const auto address = vga_text_address + static_cast<std::uint32_t>(vga_cursor_) * 2u;
        cpu.write8(address, static_cast<std::uint8_t>(' '));
        cpu.write8(address + 1u, 0x07);
    } else if (character >= 32 && character <= 126) {
        const auto address = vga_text_address + static_cast<std::uint32_t>(vga_cursor_) * 2u;
        cpu.write8(address, character);
        cpu.write8(address + 1u, 0x07);
        ++vga_cursor_;
    }

    if (vga_cursor_ >= screen_cells) {
        for (std::size_t cell = 0; cell < (vga_rows - 1) * vga_columns; ++cell) {
            const auto source = vga_text_address + static_cast<std::uint32_t>((cell + vga_columns) * 2);
            const auto target = vga_text_address + static_cast<std::uint32_t>(cell * 2);
            cpu.write8(target, cpu.read8(source));
            cpu.write8(target + 1u, cpu.read8(source + 1u));
        }
        for (std::size_t cell = (vga_rows - 1) * vga_columns; cell < screen_cells; ++cell) {
            const auto address = vga_text_address + static_cast<std::uint32_t>(cell * 2);
            cpu.write8(address, static_cast<std::uint8_t>(' '));
            cpu.write8(address + 1u, 0x07);
        }
        vga_cursor_ = static_cast<std::uint16_t>((vga_rows - 1) * vga_columns);
    }

    (void)column;
}

bool Pc::handle_bios_interrupt(Cpu& cpu, std::uint8_t vector) {
    auto& registers = cpu.registers();
    const auto get_ah = [&registers]() { return static_cast<std::uint8_t>((registers.eax >> 8) & 0xFFu); };
    const auto get_al = [&registers]() { return static_cast<std::uint8_t>(registers.eax & 0xFFu); };
    const auto set_ah = [&registers](std::uint8_t value) {
        registers.eax = (registers.eax & 0xFFFF00FFu) | (static_cast<std::uint32_t>(value) << 8);
    };
    const auto set_al = [&registers](std::uint8_t value) {
        registers.eax = (registers.eax & 0xFFFFFF00u) | value;
    };
    const auto bios_success = [this, &cpu, &set_ah, &set_al](std::uint8_t transferred = 0) {
        set_ah(0);
        set_interrupt_carry(cpu, false);
        set_al(transferred);
    };
    const auto bios_failure = [this, &cpu, &set_ah, &set_al](std::uint8_t status) {
        set_ah(status);
        set_al(0);
        set_interrupt_carry(cpu, true);
    };

    if (vector == 0x10) {
        switch (get_ah()) {
            case 0x00: { // SET VIDEO MODE
                video_mode_ = get_al();
                if (video_mode_ == 3) {
                    for (std::size_t cell = 0; cell < vga_columns * vga_rows; ++cell) {
                        const auto address = vga_text_address + static_cast<std::uint32_t>(cell * 2);
                        cpu.write8(address, static_cast<std::uint8_t>(' '));
                        cpu.write8(address + 1u, 0x07);
                    }
                    vga_cursor_ = 0;
                }
                return true;
            }
            case 0x02: { // SET CURSOR POSITION, page 0
                if (((registers.edx >> 8) & 0xFFu) < vga_rows && (registers.edx & 0xFFu) < vga_columns) {
                    vga_cursor_ = static_cast<std::uint16_t>(((registers.edx >> 8) & 0xFFu) * vga_columns +
                                                             (registers.edx & 0xFFu));
                }
                return true;
            }
            case 0x03: { // GET CURSOR POSITION, page 0
                const auto row = static_cast<std::uint8_t>(vga_cursor_ / vga_columns);
                const auto column = static_cast<std::uint8_t>(vga_cursor_ % vga_columns);
                registers.edx = (registers.edx & 0xFFFF0000u) |
                                (static_cast<std::uint32_t>(row) << 8) | column;
                registers.ecx = (registers.ecx & 0xFFFF0000u) | 0x0607u;
                return true;
            }
            case 0x0E: // TELETYPE OUTPUT
                write_teletype_character(cpu, get_al());
                return true;
            case 0x0F: // GET CURRENT VIDEO MODE
                registers.eax = (registers.eax & 0xFFFF0000u) |
                                (static_cast<std::uint32_t>(video_mode_) << 8) | vga_columns;
                registers.ebx &= 0xFFFFFF00u;
                return true;
            default:
                return false;
        }
    }

    if (vector == 0x13) {
        if ((registers.edx & 0xFFu) != 0x80u) {
            bios_failure(0x01);
            return true;
        }

        switch (get_ah()) {
            case 0x00: // RESET DISK SYSTEM
                ata_.write_port(0x3F6, 8, 0x04);
                ata_.write_port(0x3F6, 8, 0x00);
                bios_success();
                return true;

            case 0x02: { // READ CHS SECTORS
                const auto count = static_cast<std::uint16_t>(get_al());
                const auto cylinder_low = static_cast<std::uint32_t>((registers.ecx >> 8) & 0xFFu);
                const auto sector_cylinder = static_cast<std::uint32_t>(registers.ecx & 0xFFu);
                const auto cylinder = cylinder_low | ((sector_cylinder & 0xC0u) << 2);
                const auto sector = sector_cylinder & 0x3Fu;
                const auto head = static_cast<std::uint32_t>((registers.edx >> 8) & 0xFFu);
                if (count == 0 || count > 256 || sector == 0 || sector > 63 || head >= 16) {
                    bios_failure(0x04);
                    return true;
                }
                const auto lba = (cylinder * 16u + head) * 63u + sector - 1u;
                const auto destination_offset = static_cast<std::uint32_t>(registers.ebx & 0xFFFFu);
                const auto destination_segment = static_cast<std::uint32_t>(registers.es);
                const auto destination = (destination_segment << 4) + destination_offset;
                const auto byte_count = static_cast<std::uint32_t>(count) * 512u;
                if (destination_offset + byte_count > 0x10000u ||
                    destination > cpu.ram_size() || byte_count > cpu.ram_size() - destination) {
                    bios_failure(0x09);
                    return true;
                }

                const auto sectors_on_disk = static_cast<std::uint32_t>(ata_.image().size() / 512u);
                if (lba >= sectors_on_disk || count > sectors_on_disk - lba) {
                    bios_failure(0x04);
                    return true;
                }

                ata_.write_port(0x1F6, 8, 0xE0u | ((lba >> 24) & 0x0Fu));
                ata_.write_port(0x1F2, 8, count & 0xFFu);
                ata_.write_port(0x1F3, 8, lba & 0xFFu);
                ata_.write_port(0x1F4, 8, (lba >> 8) & 0xFFu);
                ata_.write_port(0x1F5, 8, (lba >> 16) & 0xFFu);
                ata_.write_port(0x1F7, 8, 0x20);
                const auto status = static_cast<std::uint8_t>(ata_.read_port(0x1F7, 8));
                if ((status & 0x01u) != 0 || (status & 0x08u) == 0) {
                    bios_failure(0x20);
                    return true;
                }

                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count) * 256u; ++i) {
                    const auto word = ata_.read_port(0x1F0, 16);
                    const auto target = destination + i * 2u;
                    cpu.write8(target, static_cast<std::uint8_t>(word & 0xFFu));
                    cpu.write8(target + 1u, static_cast<std::uint8_t>(word >> 8));
                }
                bios_success(static_cast<std::uint8_t>(count & 0xFFu));
                return true;
            }

            case 0x08: { // GET DRIVE PARAMETERS
                const auto sectors_on_disk = static_cast<std::uint32_t>(ata_.image().size() / 512u);
                if (sectors_on_disk == 0) { bios_failure(0x01); return true; }
                const auto max_cylinder = std::min<std::uint32_t>((sectors_on_disk - 1u) / (16u * 63u), 1023u);
                const auto ch = max_cylinder & 0xFFu;
                const auto cl = 63u | ((max_cylinder >> 2) & 0xC0u);
                registers.ecx = (registers.ecx & 0xFFFF0000u) | (ch << 8) | cl;
                registers.edx = (registers.edx & 0xFFFF0000u) | (15u << 8) | 1u;
                registers.ebx = (registers.ebx & 0xFFFF0000u) | 0x0004u;
                bios_success(1);
                return true;
            }

            case 0x41: { // CHECK EXTENDED DISK DRIVE SUPPORT
                if ((registers.ebx & 0xFFFFu) != 0x55AAu) { bios_failure(0x01); return true; }
                registers.ebx = (registers.ebx & 0xFFFF0000u) | 0xAA55u;
                registers.ecx = (registers.ecx & 0xFFFF0000u) | 0x0001u;
                bios_success();
                set_ah(0x30);
                return true;
            }

            case 0x42: { // EXTENDED READ: DISK ADDRESS PACKET at DS:SI
                const auto dap = (static_cast<std::uint32_t>(registers.ds) << 4) +
                                 static_cast<std::uint32_t>(registers.esi & 0xFFFFu);
                if (dap > cpu.ram_size() || 16u > cpu.ram_size() - dap || cpu.read8(dap) < 16u) {
                    bios_failure(0x01);
                    return true;
                }
                const auto count = cpu.read16(dap + 2u);
                const auto offset = cpu.read16(dap + 4u);
                const auto segment = cpu.read16(dap + 6u);
                const auto lba_low = cpu.read32(dap + 8u);
                const auto lba_high = cpu.read32(dap + 12u);
                const auto destination = (static_cast<std::uint32_t>(segment) << 4) + offset;
                const auto byte_count = static_cast<std::uint32_t>(count) * 512u;
                if (count == 0 || lba_high != 0 || (lba_low & 0xF0000000u) != 0 ||
                    (offset + byte_count > 0x10000u) ||
                    destination > cpu.ram_size() || byte_count > cpu.ram_size() - destination) {
                    bios_failure(0x01);
                    return true;
                }
                const auto sectors_on_disk = static_cast<std::uint32_t>(ata_.image().size() / 512u);
                if (lba_low >= sectors_on_disk || count > sectors_on_disk - lba_low) {
                    bios_failure(0x04);
                    return true;
                }

                ata_.write_port(0x1F6, 8, 0xE0u | ((lba_low >> 24) & 0x0Fu));
                ata_.write_port(0x1F2, 8, count & 0xFFu);
                ata_.write_port(0x1F3, 8, lba_low & 0xFFu);
                ata_.write_port(0x1F4, 8, (lba_low >> 8) & 0xFFu);
                ata_.write_port(0x1F5, 8, (lba_low >> 16) & 0xFFu);
                ata_.write_port(0x1F7, 8, 0x20);
                const auto status = static_cast<std::uint8_t>(ata_.read_port(0x1F7, 8));
                if ((status & 0x01u) != 0 || (status & 0x08u) == 0) {
                    bios_failure(0x20);
                    return true;
                }
                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count) * 256u; ++i) {
                    const auto word = ata_.read_port(0x1F0, 16);
                    const auto target = destination + i * 2u;
                    cpu.write8(target, static_cast<std::uint8_t>(word & 0xFFu));
                    cpu.write8(target + 1u, static_cast<std::uint8_t>(word >> 8));
                }
                bios_success();
                return true;
            }

            case 0x48: { // EXTENDED DRIVE PARAMETERS
                const auto table = (static_cast<std::uint32_t>(registers.ds) << 4) +
                                   static_cast<std::uint32_t>(registers.esi & 0xFFFFu);
                if (table > cpu.ram_size() || 0x1Eu > cpu.ram_size() - table ||
                    cpu.read16(table) < 0x1Au) {
                    bios_failure(0x01);
                    return true;
                }
                const auto sectors = static_cast<std::uint32_t>(ata_.image().size() / 512u);
                cpu.write16(table, 0x001Au);
                cpu.write16(table + 2u, 0x0002u);
                cpu.write32(table + 4u, std::min<std::uint32_t>((sectors - 1u) / (16u * 63u), 1023u) + 1u);
                cpu.write32(table + 8u, 16u);
                cpu.write32(table + 12u, 63u);
                cpu.write32(table + 16u, sectors);
                cpu.write32(table + 20u, 0);
                cpu.write16(table + 24u, 512u);
                bios_success();
                return true;
            }

            default:
                bios_failure(0x01);
                return true;
        }
    }

    if (vector == 0x15) {
        if (registers.eax == 0x0000E820u) { // GET SYSTEM MEMORY MAP
            if (registers.edx != 0x534D4150u || registers.ecx < 20u) {
                bios_failure(0x86);
                return true;
            }
            struct MapEntry { std::uint32_t base, length, type; };
            const std::array<MapEntry, 3> entries{{
                {0x00000000u, 0x0009FC00u, 1u},
                {0x0009FC00u, 0x00060400u, 2u},
                {0x00100000u, static_cast<std::uint32_t>(cpu.ram_size() - 0x00100000u), 1u}
            }};
            const auto index = registers.ebx;
            if (index >= entries.size()) { bios_failure(0x86); return true; }
            const auto destination = (static_cast<std::uint32_t>(registers.es) << 4) +
                                     static_cast<std::uint32_t>(registers.edi & 0xFFFFu);
            if (destination > cpu.ram_size() || 20u > cpu.ram_size() - destination) {
                bios_failure(0x86);
                return true;
            }
            const auto& entry = entries[index];
            cpu.write32(destination, entry.base);
            cpu.write32(destination + 4u, 0);
            cpu.write32(destination + 8u, entry.length);
            cpu.write32(destination + 12u, 0);
            cpu.write32(destination + 16u, entry.type);
            registers.eax = 0x534D4150u; // "SMAP"
            registers.ecx = 20u;
            registers.ebx = index + 1u < entries.size() ? index + 1u : 0u;
            set_interrupt_carry(cpu, false);
            return true;
        }

        if ((registers.eax & 0xFFFFu) == 0xE801u) { // GET EXTENDED MEMORY SIZE, E801h
            constexpr std::size_t one_mib = 1024u * 1024u;
            constexpr std::size_t sixteen_mib = 16u * one_mib;
            const auto ram_bytes = cpu.ram_size();
            const auto below_16_mib_kib = std::min<std::size_t>(
                ram_bytes > one_mib ? ram_bytes - one_mib : 0u, 15u * one_mib) / 1024u;
            const auto above_16_mib_blocks = ram_bytes > sixteen_mib
                ? (ram_bytes - sixteen_mib) / 65536u : 0u;
            const auto ax = static_cast<std::uint16_t>(std::min<std::size_t>(below_16_mib_kib, 0xFFFFu));
            const auto bx = static_cast<std::uint16_t>(std::min<std::size_t>(above_16_mib_blocks, 0xFFFFu));
            registers.eax = (registers.eax & 0xFFFF0000u) | ax;
            registers.ecx = (registers.ecx & 0xFFFF0000u) | ax;
            registers.ebx = (registers.ebx & 0xFFFF0000u) | bx;
            registers.edx = (registers.edx & 0xFFFF0000u) | bx;
            set_interrupt_carry(cpu, false);
            return true;
        }

        if (get_ah() == 0x88) { // GET EXTENDED MEMORY SIZE, in KiB above 1 MiB
            const auto kib = (cpu.ram_size() - 0x00100000u) / 1024u;
            registers.eax = (registers.eax & 0xFFFF0000u) |
                            static_cast<std::uint16_t>(std::min<std::size_t>(kib, 0xFFFFu));
            set_interrupt_carry(cpu, false);
            return true;
        }

        bios_failure(0x86);
        return true;
    }

    return false;
}

std::size_t Pc::run(std::size_t instruction_limit) {
    if (!booted_) throw std::logic_error("call boot() before running the PC");
    std::size_t executed = 0;
    while (executed < instruction_limit && !cpu_.halted()) {
        cpu_.step();
        ++executed;
    }
    return executed;
}

std::string Pc::text_screen() const {
    if (cpu_.ram_size() < static_cast<std::size_t>(vga_text_address) + vga_bytes)
        return {};

    std::string output;
    for (std::size_t row = 0; row < vga_rows; ++row) {
        std::string line;
        line.reserve(vga_columns);
        for (std::size_t column = 0; column < vga_columns; ++column) {
            const auto cell = row * vga_columns + column;
            const auto value = cpu_.read8(vga_text_address + static_cast<std::uint32_t>(cell * 2));
            line.push_back(value >= 32 && value <= 126 ? static_cast<char>(value) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) {
            output += line;
            output.push_back('\n');
        }
    }
    return output;
}

std::vector<std::uint8_t> make_demo_disk_image() {
    constexpr std::size_t disk_bytes = 8u * 1024u * 1024u;
    constexpr std::size_t sector_bytes = AtaPioDevice::sector_bytes;
    constexpr std::size_t stage_two_message_offset = 0x20;
    constexpr std::string_view message = "XP REVIVAL - ATA BOOT OK";

    std::vector<std::uint8_t> image(disk_bytes, 0);

    // Stage 1 (LBA 0): program the primary-master task file for an LBA 1
    // read, then REP INSW transfers all 256 words into 0000:8000. A far jump
    // transfers control to stage 2. No host-side second-stage loading occurs.
    constexpr std::uint8_t stage_one[] = {
        0xBA, 0xF6, 0x01,       // MOV DX,1F6h
        0xB0, 0xE0,             // MOV AL,E0h (primary master, LBA mode)
        0xEE,                   // OUT DX,AL
        0xBA, 0xF2, 0x01,       // MOV DX,1F2h
        0xB0, 0x01,             // MOV AL,1 sector
        0xEE,                   // OUT DX,AL
        0xBA, 0xF3, 0x01,       // MOV DX,1F3h
        0xB0, 0x01,             // MOV AL, LBA low = 1
        0xEE,                   // OUT DX,AL
        0xBA, 0xF4, 0x01,       // MOV DX,1F4h
        0xB0, 0x00,             // MOV AL, LBA mid = 0
        0xEE,                   // OUT DX,AL
        0xBA, 0xF5, 0x01,       // MOV DX,1F5h
        0xB0, 0x00,             // MOV AL, LBA high = 0
        0xEE,                   // OUT DX,AL
        0xBA, 0xF7, 0x01,       // MOV DX,1F7h
        0xB0, 0x20,             // MOV AL,20h (READ SECTORS)
        0xEE,                   // OUT DX,AL
        0xBA, 0xF0, 0x01,       // MOV DX,1F0h (data register)
        0xBF, 0x00, 0x80,       // MOV DI,8000h
        0xB9, 0x00, 0x01,       // MOV CX,256 words
        0xF3, 0x6D,             // REP INSW
        0xEA, 0x00, 0x80, 0x00, 0x00 // JMP FAR 0000:8000
    };
    std::copy(std::begin(stage_one), std::end(stage_one), image.begin());

    // Stage 2 (LBA 1): copy VGA character/attribute words to B800:0000.
    constexpr std::uint8_t stage_two[] = {
        0xB8, 0x00, 0xB8,       // MOV AX,B800h
        0x8E, 0xC0,             // MOV ES,AX
        0xBE, 0x20, 0x80,       // MOV SI,8020h
        0xBF, 0x00, 0x00,       // MOV DI,0000h
        0xB9, 0x18, 0x00,       // MOV CX,24
        0xFC,                   // CLD
        0xF3, 0xA5,             // REP MOVSW
        0xF4                    // HLT
    };
    std::copy(std::begin(stage_two), std::end(stage_two), image.begin() + sector_bytes);

    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto offset = sector_bytes + stage_two_message_offset + i * 2;
        image[offset] = static_cast<std::uint8_t>(message[i]);
        image[offset + 1] = 0x07; // VGA light-grey-on-black attribute
    }

    image[510] = 0x55;
    image[511] = 0xAA;
    return image;
}
} // namespace xprevival
