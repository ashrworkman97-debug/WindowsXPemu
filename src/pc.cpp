#include "xprevival/pc.hpp"

#include <algorithm>
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
    constexpr std::size_t message_offset = 0x40;
    constexpr std::string_view message = "XP REVIVAL - ATA BOOT OK";

    std::vector<std::uint8_t> image(disk_bytes, 0);
    auto* sector = image.data();

    // 16-bit boot sector:
    //   ES = B800h; REP MOVSW copies character/attribute cells to text memory.
    //   Then IDENTIFY DEVICE is issued to the primary-master ATA device and
    //   word 0 (0040h) is stored at 0000:0600 before halting.
    constexpr std::uint8_t program[] = {
        0xB8, 0x00, 0xB8,       // MOV AX,B800h
        0x8E, 0xC0,             // MOV ES,AX
        0xBE, 0x40, 0x7C,       // MOV SI,7C40h
        0xBF, 0x00, 0x00,       // MOV DI,0000h
        0xB9, 0x18, 0x00,       // MOV CX,24
        0xFC,                   // CLD
        0xF3, 0xA5,             // REP MOVSW
        0xBA, 0xF6, 0x01,       // MOV DX,1F6h
        0xB0, 0xE0,             // MOV AL,E0h (primary master, LBA mode)
        0xEE,                   // OUT DX,AL
        0xBA, 0xF7, 0x01,       // MOV DX,1F7h
        0xB0, 0xEC,             // MOV AL,ECh (IDENTIFY DEVICE)
        0xEE,                   // OUT DX,AL
        0xBA, 0xF0, 0x01,       // MOV DX,1F0h
        0xED,                   // IN AX,DX
        0xA3, 0x00, 0x06,       // MOV [0600h],AX
        0xF4                    // HLT
    };
    std::copy(std::begin(program), std::end(program), sector);

    for (std::size_t i = 0; i < message.size(); ++i) {
        sector[message_offset + i * 2] = static_cast<std::uint8_t>(message[i]);
        sector[message_offset + i * 2 + 1] = 0x07; // VGA light-grey-on-black attribute
    }

    sector[510] = 0x55;
    sector[511] = 0xAA;
    return image;
}

} // namespace xprevival
