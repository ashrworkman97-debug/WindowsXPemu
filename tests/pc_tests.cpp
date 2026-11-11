#include "xprevival/pc.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)
}

int main() {
    using xprevival::Pc;

    Pc pc(xprevival::make_demo_disk_image(), 4u * 1024u * 1024u);
    CHECK(!pc.booted());

    bool rejected_run_before_boot = false;
    try { (void)pc.run(1); }
    catch (const std::logic_error&) { rejected_run_before_boot = true; }
    CHECK(rejected_run_before_boot);

    pc.boot();
    CHECK(pc.booted());
    CHECK(pc.cpu().registers().cs == 0);
    CHECK(pc.cpu().registers().eip == 0x7C00);
    CHECK((pc.cpu().registers().edx & 0xFFu) == 0x80u);

    const auto executed = pc.run(128);
    CHECK(executed > 10);
    CHECK(pc.cpu().halted());
    CHECK(pc.cpu().last_error().empty());
    CHECK(pc.cpu().read16(0x8000) == 0x00B8); // stage two was fetched from LBA 1
    CHECK((pc.disk().status() & 0x08u) == 0); // ATA data request cleared after 256 words
    CHECK(pc.text_screen().find("XP REVIVAL - ATA BOOT OK") != std::string::npos);

    bool rejected_second_boot = false;
    try { pc.boot(); }
    catch (const std::logic_error&) { rejected_second_boot = true; }
    CHECK(rejected_second_boot);

    auto invalid_image = xprevival::make_demo_disk_image();
    invalid_image[510] = 0;
    Pc invalid_pc(std::move(invalid_image), 2u * 1024u * 1024u);
    bool rejected_missing_signature = false;
    try { invalid_pc.boot(); }
    catch (const std::runtime_error&) { rejected_missing_signature = true; }
    CHECK(rejected_missing_signature);

    // BIOS INT 10h teletype output is serviced by the PC firmware model.
    std::vector<std::uint8_t> video_disk(2u * 512u, 0);
    constexpr std::array<std::uint8_t, 7> teletype_program{
        0xB4, 0x0E,       // MOV AH,0Eh
        0xB0, 0x42,       // MOV AL,'B'
        0xCD, 0x10,       // INT 10h
        0xF4              // HLT
    };
    std::copy(teletype_program.begin(), teletype_program.end(), video_disk.begin());
    video_disk[510] = 0x55; video_disk[511] = 0xAA;
    Pc video_pc(std::move(video_disk), 2u * 1024u * 1024u);
    video_pc.boot();
    (void)video_pc.run(16);
    CHECK(video_pc.cpu().halted());
    CHECK(video_pc.cpu().read8(0xB8000) == static_cast<std::uint8_t>('B'));
    CHECK(video_pc.text_screen().find('B') != std::string::npos);
    CHECK(video_pc.cpu().registers().esp == 0x7C00);

    // BIOS INT 13h AH=02 reads CHS sector 2 through the attached ATA PIO device.
    std::vector<std::uint8_t> bios_disk(4u * 512u, 0);
    for (std::size_t i = 0; i < 512; ++i)
        bios_disk[512 + i] = static_cast<std::uint8_t>((i * 7u) & 0xFFu);
    constexpr std::array<std::uint8_t, 16> bios_read_program{
        0xB8, 0x01, 0x02, // MOV AX,0201h (read one sector)
        0xBB, 0x00, 0x80, // MOV BX,8000h (buffer)
        0xB9, 0x02, 0x00, // MOV CX,0002h (cylinder 0, sector 2)
        0xBA, 0x80, 0x00, // MOV DX,0080h (head 0, disk 80h)
        0xCD, 0x13,       // INT 13h
        0xF4,             // HLT
        0x90              // NOP padding
    };
    std::copy(bios_read_program.begin(), bios_read_program.end(), bios_disk.begin());
    bios_disk[510] = 0x55; bios_disk[511] = 0xAA;
    Pc bios_disk_pc(std::move(bios_disk), 2u * 1024u * 1024u);
    bios_disk_pc.boot();
    (void)bios_disk_pc.run(32);
    CHECK(bios_disk_pc.cpu().halted());
    CHECK(bios_disk_pc.cpu().last_error().empty());
    CHECK((bios_disk_pc.cpu().registers().eflags & xprevival::Cpu::Carry) == 0);
    CHECK((bios_disk_pc.cpu().registers().eax & 0xFFFFu) == 1u);
    CHECK(bios_disk_pc.cpu().read8(0x8000) == 0);
    CHECK(bios_disk_pc.cpu().read8(0x8001) == 7);
    CHECK(bios_disk_pc.cpu().read8(0x81FE) == static_cast<std::uint8_t>((510u * 7u) & 0xFFu));
    CHECK((bios_disk_pc.disk().status() & 0x08u) == 0);

    // BIOS INT 15h E820 returns a 20-byte usable-memory map entry.
    std::vector<std::uint8_t> e820_disk(2u * 512u, 0);
    constexpr std::array<std::uint8_t, 24> e820_program{
        0x66, 0xB8, 0x20, 0xE8, 0x00, 0x00, // MOV EAX,E820h
        0x66, 0xBA, 0x50, 0x41, 0x4D, 0x53, // MOV EDX,'SMAP'
        0xBF, 0x00, 0x05,                   // MOV DI,0500h
        0xB9, 0x14, 0x00,                   // MOV CX,20
        0xBB, 0x00, 0x00,                   // MOV BX,0 (first map entry)
        0xCD, 0x15,                         // INT 15h
        0xF4                                // HLT
    };
    std::copy(e820_program.begin(), e820_program.end(), e820_disk.begin());
    e820_disk[510] = 0x55; e820_disk[511] = 0xAA;
    Pc e820_pc(std::move(e820_disk), 4u * 1024u * 1024u);
    e820_pc.boot();
    (void)e820_pc.run(32);
    CHECK(e820_pc.cpu().halted());
    CHECK(e820_pc.cpu().last_error().empty());
    CHECK(e820_pc.cpu().registers().eax == 0x534D4150u);
    CHECK(e820_pc.cpu().registers().ecx == 20u);
    CHECK(e820_pc.cpu().registers().ebx == 1u);
    CHECK(e820_pc.cpu().read32(0x500) == 0u);
    CHECK(e820_pc.cpu().read32(0x508) == 0x0009FC00u);
    CHECK(e820_pc.cpu().read32(0x510) == 1u);
    CHECK((e820_pc.cpu().registers().eflags & xprevival::Cpu::Carry) == 0);

    // BIOS INT 13h AH=42 reads an LBA sector through an EDD disk address packet.
    std::vector<std::uint8_t> edd_disk(4u * 512u, 0);
    for (std::size_t i = 0; i < 512; ++i)
        edd_disk[512 + i] = static_cast<std::uint8_t>(0xFFu - (i & 0xFFu));
    constexpr std::array<std::uint8_t, 9> edd_program{
        0xB8, 0x00, 0x42, // MOV AX,4200h
        0xBE, 0x40, 0x7C, // MOV SI,7C40h (DAP)
        0xCD, 0x13,       // INT 13h
        0xF4              // HLT
    };
    std::copy(edd_program.begin(), edd_program.end(), edd_disk.begin());
    constexpr std::array<std::uint8_t, 16> dap{
        0x10, 0x00,       // packet size
        0x01, 0x00,       // one sector
        0x00, 0x80,       // buffer offset 8000h
        0x00, 0x00,       // buffer segment 0000h
        0x01, 0x00, 0x00, 0x00, // LBA low 32
        0x00, 0x00, 0x00, 0x00  // LBA high 32
    };
    std::copy(dap.begin(), dap.end(), edd_disk.begin() + 0x40);
    edd_disk[510] = 0x55; edd_disk[511] = 0xAA;
    Pc edd_pc(std::move(edd_disk), 2u * 1024u * 1024u);
    edd_pc.boot();
    (void)edd_pc.run(32);
    CHECK(edd_pc.cpu().halted());
    CHECK(edd_pc.cpu().last_error().empty());
    CHECK((edd_pc.cpu().registers().eflags & xprevival::Cpu::Carry) == 0);
    CHECK((edd_pc.cpu().registers().eax & 0xFF00u) == 0);
    CHECK(edd_pc.cpu().read8(0x8000) == 0xFFu);
    CHECK(edd_pc.cpu().read8(0x8001) == 0xFEu);
    CHECK(edd_pc.cpu().read8(0x81FF) == 0x00u);

    // BIOS INT 15h E801 returns memory below and above 16 MiB in legacy units.
    std::vector<std::uint8_t> e801_disk(2u * 512u, 0);
    constexpr std::array<std::uint8_t, 6> e801_program{
        0xB8, 0x01, 0xE8, // MOV AX,E801h
        0xCD, 0x15,       // INT 15h
        0xF4              // HLT
    };
    std::copy(e801_program.begin(), e801_program.end(), e801_disk.begin());
    e801_disk[510] = 0x55; e801_disk[511] = 0xAA;
    Pc e801_pc(std::move(e801_disk), 4u * 1024u * 1024u);
    e801_pc.boot();
    (void)e801_pc.run(16);
    CHECK(e801_pc.cpu().halted());
    CHECK(e801_pc.cpu().last_error().empty());
    CHECK((e801_pc.cpu().registers().eax & 0xFFFFu) == 3072u);
    CHECK((e801_pc.cpu().registers().ebx & 0xFFFFu) == 0u);
    CHECK((e801_pc.cpu().registers().ecx & 0xFFFFu) == 3072u);
    CHECK((e801_pc.cpu().registers().edx & 0xFFFFu) == 0u);
    CHECK((e801_pc.cpu().registers().eflags & xprevival::Cpu::Carry) == 0);

    if (failures != 0) {
        std::cerr << failures << " PC test(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "PC boot tests passed.\n";
    return EXIT_SUCCESS;
}
