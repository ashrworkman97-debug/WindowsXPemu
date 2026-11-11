#include "xprevival/ata_pio.hpp"
#include "xprevival/cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void install_paging_bootstrap(xprevival::Cpu& cpu, std::span<const std::uint8_t> protected_code,
                              bool map_test_page) {
    constexpr std::array<std::uint8_t, 8> code_descriptor{
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00
    };
    constexpr std::array<std::uint8_t, 8> data_descriptor{
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00
    };
    for (std::size_t i = 0; i < 8; ++i) {
        cpu.write8(0x0508u + static_cast<std::uint32_t>(i), code_descriptor[i]);
        cpu.write8(0x0510u + static_cast<std::uint32_t>(i), data_descriptor[i]);
    }
    cpu.write16(0x0600, 0x0017);
    cpu.write32(0x0602, 0x00000500);

    constexpr std::array<std::uint8_t, 22> real_mode_bootstrap{
        0x0F, 0x01, 0x16, 0x00, 0x06,
        0x0F, 0x20, 0xC0,
        0x66, 0x0D, 0x01, 0x00, 0x00, 0x00,
        0x0F, 0x22, 0xC0,
        0xEA, 0x20, 0x7C, 0x08, 0x00
    };
    cpu.load(real_mode_bootstrap, 0x7C00);
    cpu.load(protected_code, 0x7C20);

    cpu.write32(0x1000, 0x00002003); // PDE 0
    cpu.write32(0x1004, 0x00003003); // PDE 1
    cpu.write32(0x2000, 0x00000003); // identity map page 0
    cpu.write32(0x201C, 0x00007003); // identity map code page 7
    if (map_test_page) cpu.write32(0x300C, 0x00008003); // 00403000h -> 8000h
    cpu.write32(0x8000, 0xCAFEBABEu);

    auto& regs = cpu.registers();
    regs.cs = 0;
    regs.ds = 0;
    regs.ss = 0;
    regs.eip = 0x7C00;
}

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "CHECK failed on line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)
}

int main() {
    using xprevival::AtaPioDevice;
    using xprevival::Cpu;
    Cpu cpu(4u * 1024u * 1024u);
    CHECK(cpu.ram_size() == 4u * 1024u * 1024u);
    CHECK(cpu.registers().cs == 0xF000);
    CHECK(cpu.registers().eip == 0xFFF0);

    // 16-bit real-mode subset: MOV AX,1234h ; MOV BX,5678h ; NOP ; HLT
    constexpr std::array<std::uint8_t, 8> real_mode_program{
        0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56, 0x90, 0xF4
    };
    cpu.load(real_mode_program, 0x7C00);
    auto& regs = cpu.registers();
    regs.cs = 0;
    regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.registers().eax == 0x00001234);
    CHECK(cpu.registers().ebx == 0x00005678);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // 386 operand-size override: EAX=12345678h; EBX=1; ADD; CMP; JZ; HLT.
    cpu.reset();
    constexpr std::array<std::uint8_t, 27> operand_size_program{
        0x66, 0xB8, 0x78, 0x56, 0x34, 0x12,
        0x66, 0xBB, 0x01, 0x00, 0x00, 0x00,
        0x66, 0x01, 0xD8,
        0x66, 0x3D, 0x79, 0x56, 0x34, 0x12,
        0x74, 0x03,
        0xB8, 0x00, 0x00,
        0xF4
    };
    cpu.load(operand_size_program, 0x7C00);
    regs.cs = 0;
    regs.eip = 0x7C00;
    cpu.run(32);
    CHECK(cpu.registers().eax == 0x12345679);
    CHECK(cpu.registers().ebx == 1);
    CHECK((cpu.registers().eflags & Cpu::Zero) != 0);
    CHECK(cpu.registers().eip == 0x7C1B);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // 16-bit ModRM memory operands and byte-register writes (AL/AH).
    cpu.reset();
    constexpr std::array<std::uint8_t, 15> memory_program{
        0xB8, 0x34, 0x12,       // MOV AX,1234h
        0xBB, 0x00, 0x02,       // MOV BX,0200h
        0x89, 0x07,             // MOV [BX],AX
        0x8B, 0x0F,             // MOV CX,[BX]
        0xB0, 0x7F,             // MOV AL,7Fh
        0xB4, 0x12,             // MOV AH,12h
        0xF4                    // HLT
    };
    cpu.load(memory_program, 0x7C00);
    regs.cs = 0;
    regs.ds = 0;
    regs.ss = 0;
    regs.eip = 0x7C00;
    cpu.run(32);
    CHECK(cpu.read16(0x0200) == 0x1234);
    CHECK(cpu.registers().ecx == 0x1234);
    CHECK(cpu.registers().eax == 0x127F);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // BP-based addressing defaults to SS; an explicit 3Eh prefix selects DS.
    cpu.reset();
    constexpr std::array<std::uint8_t, 8> segment_program{
        0x89, 0x46, 0x00,       // MOV [BP+0],AX -> SS
        0x3E, 0x89, 0x46, 0x00, // MOV DS:[BP+0],AX
        0xF4
    };
    cpu.load(segment_program, 0x7C00);
    regs.cs = 0;
    regs.ds = 0x1000;
    regs.ss = 0x2000;
    regs.ebp = 0x0020;
    regs.eax = 0xBEEF;
    regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.read16(0x20020) == 0xBEEF);
    CHECK(cpu.read16(0x10020) == 0xBEEF);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // 386-width data transfer to/from memory using a 16-bit effective address.
    cpu.reset();
    constexpr std::array<std::uint8_t, 17> dword_memory_program{
        0x66, 0xB8, 0x78, 0x56, 0x34, 0x12, // MOV EAX,12345678h
        0x66, 0x89, 0x06, 0x00, 0x03,       // MOV [0300h],EAX
        0x66, 0x8B, 0x1E, 0x00, 0x03,       // MOV EBX,[0300h]
        0xF4                                  // HLT
    };
    cpu.load(dword_memory_program, 0x7C00);
    regs.cs = 0;
    regs.ds = 0;
    regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.read32(0x0300) == 0x12345678);
    CHECK(cpu.registers().ebx == 0x12345678);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // 8-bit arithmetic must set flags at 8-bit width, not 16-bit width.
    cpu.reset();
    constexpr std::array<std::uint8_t, 15> byte_arithmetic_program{
        0xB0, 0xFF, // MOV AL,FFh
        0xB3, 0x01, // MOV BL,1
        0x00, 0xD8, // ADD AL,BL -> 00h, CF=1, ZF=1
        0x72, 0x02, // JB over the marker MOV
        0xB0, 0x55, // must be skipped
        0x74, 0x02, // JZ over the second marker MOV
        0xB0, 0x66, // must be skipped
        0xF4         // HLT
    };
    cpu.load(byte_arithmetic_program, 0x7C00);
    regs.cs = 0;
    regs.eip = 0x7C00;
    cpu.run(8);
    CHECK((cpu.registers().eax & 0xFFu) == 0);
    CHECK((cpu.registers().eflags & Cpu::Carry) != 0);
    CHECK((cpu.registers().eflags & Cpu::Zero) != 0);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());


    // 386 protected mode + 4 KiB paging: access a non-identity-mapped page,
    // then write it and inspect the page-table accessed/dirty bits.
    cpu.reset();
    constexpr std::array<std::uint8_t, 38> paged_program{
        0xB8, 0x10, 0x00, 0x00, 0x00, // MOV EAX,0010h
        0x8E, 0xD8,                   // MOV DS,AX
        0x8E, 0xD0,                   // MOV SS,AX
        0xB8, 0x00, 0x10, 0x00, 0x00, // MOV EAX,1000h
        0x0F, 0x22, 0xD8,             // MOV CR3,EAX
        0xB8, 0x01, 0x00, 0x00, 0x80, // MOV EAX,80000001h
        0x0F, 0x22, 0xC0,             // MOV CR0,EAX (enable paging)
        0x8B, 0x05, 0x00, 0x30, 0x40, 0x00, // MOV EAX,[00403000h]
        0x89, 0x05, 0x04, 0x30, 0x40, 0x00, // MOV [00403004h],EAX
        0xF4                          // HLT
    };
    install_paging_bootstrap(cpu, paged_program, true);
    cpu.run(64);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());
    CHECK(cpu.registers().eax == 0xCAFEBABEu);
    CHECK(cpu.read32(0x8004) == 0xCAFEBABEu);
    CHECK((cpu.registers().cr0 & 0x80000001u) == 0x80000001u);
    CHECK((cpu.read32(0x1000) & 0x20u) != 0); // PDE accessed
    CHECK((cpu.read32(0x201C) & 0x20u) != 0); // code PTE accessed
    CHECK((cpu.read32(0x1004) & 0x20u) != 0); // data PDE accessed
    CHECK((cpu.read32(0x300C) & 0x60u) == 0x60u); // data PTE accessed + dirty

    // An absent PTE records CR2 and the non-present read page-fault code.
    cpu.reset();
    constexpr std::array<std::uint8_t, 31> fault_program{
        0xB8, 0x10, 0x00, 0x00, 0x00, // MOV EAX,0010h
        0x8E, 0xD8,                   // MOV DS,AX
        0x8E, 0xD0,                   // MOV SS,AX
        0xB8, 0x00, 0x10, 0x00, 0x00, // MOV EAX,1000h
        0x0F, 0x22, 0xD8,             // MOV CR3,EAX
        0xB8, 0x01, 0x00, 0x00, 0x80, // MOV EAX,80000001h
        0x0F, 0x22, 0xC0,             // MOV CR0,EAX
        0x8B, 0x05, 0x00, 0x00, 0x40, 0x00 // MOV EAX,[00400000h]
    };
    install_paging_bootstrap(cpu, fault_program, false);
    cpu.run(64);
    CHECK(cpu.halted());
    CHECK(cpu.registers().cr2 == 0x00400000u);
    CHECK(cpu.last_page_fault_error_code() == 0u);
    CHECK(cpu.last_error().find("page fault") != std::string::npos);


    // REP MOVSB copies CX bytes and advances SI/DI using the active direction flag.
    cpu.reset();
    constexpr std::array<std::uint8_t, 12> rep_movsb_program{
        0xB9, 0x03, 0x00, // MOV CX,3
        0xBE, 0x00, 0x03, // MOV SI,0300h
        0xBF, 0x00, 0x04, // MOV DI,0400h
        0xF3, 0xA4,       // REP MOVSB
        0xF4
    };
    cpu.write8(0x0300, 0xA1);
    cpu.write8(0x0301, 0xB2);
    cpu.write8(0x0302, 0xC3);
    cpu.load(rep_movsb_program, 0x7C00);
    regs.cs = 0; regs.ds = 0; regs.es = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.read8(0x0400) == 0xA1);
    CHECK(cpu.read8(0x0401) == 0xB2);
    CHECK(cpu.read8(0x0402) == 0xC3);
    CHECK((cpu.registers().ecx & 0xFFFFu) == 0);
    CHECK((cpu.registers().esi & 0xFFFFu) == 0x0303);
    CHECK((cpu.registers().edi & 0xFFFFu) == 0x0403);
    CHECK(cpu.halted());


    // REP STOSB must store AL (not AX) to each ES:DI byte.
    cpu.reset();
    constexpr std::array<std::uint8_t, 11> rep_stosb_program{
        0xB9, 0x03, 0x00, // MOV CX,3
        0xB0, 0x5A,       // MOV AL,5Ah
        0xBF, 0x00, 0x05, // MOV DI,0500h
        0xF3, 0xAA,       // REP STOSB
        0xF4
    };
    cpu.load(rep_stosb_program, 0x7C00);
    regs.cs = 0; regs.es = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.read8(0x0500) == 0x5A);
    CHECK(cpu.read8(0x0501) == 0x5A);
    CHECK(cpu.read8(0x0502) == 0x5A);
    CHECK((cpu.registers().ecx & 0xFFFFu) == 0);
    CHECK((cpu.registers().edi & 0xFFFFu) == 0x0503);
    CHECK(cpu.halted());

    // STD makes REP MOVSB walk backwards.
    cpu.reset();
    constexpr std::array<std::uint8_t, 13> backwards_program{
        0xBE, 0x02, 0x03, // MOV SI,0302h
        0xBF, 0x02, 0x04, // MOV DI,0402h
        0xB9, 0x03, 0x00, // MOV CX,3
        0xFD,             // STD
        0xF3, 0xA4,       // REP MOVSB
        0xF4
    };
    cpu.write8(0x0300, 0x11);
    cpu.write8(0x0301, 0x22);
    cpu.write8(0x0302, 0x33);
    cpu.load(backwards_program, 0x7C00);
    regs.cs = 0; regs.ds = 0; regs.es = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(cpu.read8(0x0400) == 0x11);
    CHECK(cpu.read8(0x0401) == 0x22);
    CHECK(cpu.read8(0x0402) == 0x33);
    CHECK((cpu.registers().esi & 0xFFFFu) == 0x02FF);
    CHECK((cpu.registers().edi & 0xFFFFu) == 0x03FF);
    CHECK((cpu.registers().eflags & Cpu::Direction) != 0);
    CHECK(cpu.halted());

    // CPUID reports the fixed virtual Intel CPU identity/features.
    cpu.reset();
    constexpr std::array<std::uint8_t, 8> cpuid_program{
        0xB8, 0x00, 0x00, 0x00, 0x00, // MOV EAX,0
        0x0F, 0xA2,                   // CPUID
        0xF4
    };
    cpu.load(cpuid_program, 0x7C00);
    regs.cs = 0; regs.eip = 0x7C00;
    cpu.run(8);
    CHECK(cpu.registers().eax == 1);
    CHECK(cpu.registers().ebx == 0x756E6547u);
    CHECK(cpu.registers().edx == 0x49656E69u);
    CHECK(cpu.registers().ecx == 0x6C65746Eu);
    CHECK(cpu.halted());



    // CPUID's feature mask advertises the implemented TSC and no unmodeled extensions.
    cpu.reset();
    constexpr std::array<std::uint8_t, 8> cpuid_features_program{
        0xB8, 0x01, 0x00, 0x00, 0x00, // MOV EAX,1
        0x0F, 0xA2,                   // CPUID
        0xF4
    };
    cpu.load(cpuid_features_program, 0x7C00);
    regs.cs = 0; regs.eip = 0x7C00;
    cpu.run(8);
    CHECK(cpu.registers().eax == 0x00000673u);
    CHECK(cpu.registers().edx == (1u << 4)); // TSC only
    CHECK(cpu.halted());

    // REPE CMPSB stops at the first mismatch and leaves the mismatch flags.
    cpu.reset();
    constexpr std::array<std::uint8_t, 13> repe_cmpsb_program{
        0xBE, 0x00, 0x03, // MOV SI,0300h
        0xBF, 0x00, 0x04, // MOV DI,0400h
        0xB9, 0x03, 0x00, // MOV CX,3
        0xF3, 0xA6,       // REPE CMPSB
        0xF4,             // HLT
        0x90
    };
    cpu.write8(0x0300, 0x11); cpu.write8(0x0301, 0x22); cpu.write8(0x0302, 0x33);
    cpu.write8(0x0400, 0x11); cpu.write8(0x0401, 0x99); cpu.write8(0x0402, 0x33);
    cpu.load(repe_cmpsb_program, 0x7C00);
    regs.cs = 0; regs.ds = 0; regs.es = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK((cpu.registers().ecx & 0xFFFFu) == 1);
    CHECK((cpu.registers().esi & 0xFFFFu) == 0x0302);
    CHECK((cpu.registers().edi & 0xFFFFu) == 0x0402);
    CHECK((cpu.registers().eflags & Cpu::Zero) == 0);
    CHECK(cpu.halted());

    // REPE SCASB uses AL (not AX) while scanning ES:DI.
    cpu.reset();
    constexpr std::array<std::uint8_t, 12> repe_scasb_program{
        0xB0, 0x5A,       // MOV AL,5Ah
        0xBF, 0x00, 0x05, // MOV DI,0500h
        0xB9, 0x02, 0x00, // MOV CX,2
        0xF3, 0xAE,       // REPE SCASB
        0xF4,
        0x90
    };
    cpu.write8(0x0500, 0x5A); cpu.write8(0x0501, 0x6B);
    cpu.load(repe_scasb_program, 0x7C00);
    regs.cs = 0; regs.es = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK((cpu.registers().ecx & 0xFFFFu) == 0);
    CHECK((cpu.registers().edi & 0xFFFFu) == 0x0502);
    CHECK((cpu.registers().eax & 0xFFu) == 0x5A);
    CHECK((cpu.registers().eflags & Cpu::Zero) == 0);
    CHECK(cpu.halted());

    // RDTSC advances a deterministic virtual counter rather than wall-clock time.
    cpu.reset();
    constexpr std::array<std::uint8_t, 3> rdtsc_program{0x0F, 0x31, 0xF4};
    cpu.load(rdtsc_program, 0x7C00);
    regs.cs = 0; regs.eip = 0x7C00;
    cpu.run(8);
    CHECK(cpu.registers().eax == 1);
    CHECK(cpu.registers().edx == 0);
    CHECK(cpu.halted());

    // Port I/O is delegated to the PC/device bus callbacks.
    cpu.reset();
    std::uint16_t observed_port = 0;
    std::uint32_t observed_value = 0;
    cpu.set_io_callbacks(
        [&](std::uint16_t port, unsigned width) {
            CHECK(port == 0x1234);
            CHECK(width == 8);
            return observed_value;
        },
        [&](std::uint16_t port, unsigned width, std::uint32_t value) {
            observed_port = port;
            CHECK(width == 8);
            observed_value = value;
        });
    constexpr std::array<std::uint8_t, 8> io_program{
        0xBA, 0x34, 0x12, // MOV DX,1234h
        0xB0, 0x5A,       // MOV AL,5Ah
        0xEE,             // OUT DX,AL
        0xEC,             // IN AL,DX
        0xF4
    };
    cpu.load(io_program, 0x7C00);
    regs.cs = 0; regs.eip = 0x7C00;
    cpu.run(16);
    CHECK(observed_port == 0x1234);
    CHECK(observed_value == 0x5A);
    CHECK((cpu.registers().eax & 0xFFu) == 0x5A);
    CHECK(cpu.halted());

    // CLI/STI and CLD/STD expose the basic IF and DF controls.
    cpu.reset();
    constexpr std::array<std::uint8_t, 5> flags_program{0xFA, 0xFD, 0xFB, 0xFC, 0xF4};
    cpu.load(flags_program, 0x7C00);
    regs.cs = 0; regs.eip = 0x7C00;
    cpu.run(8);
    CHECK((cpu.registers().eflags & Cpu::Interrupt) != 0);
    CHECK((cpu.registers().eflags & Cpu::Direction) == 0);
    CHECK(cpu.halted());



    // Real-mode INT pushes an architectural frame; IRET restores CS:IP and FLAGS.
    cpu.reset();
    constexpr std::array<std::uint8_t, 3> int_program{0xCD, 0x10, 0xF4};
    constexpr std::array<std::uint8_t, 4> int_handler{
        0xB8, 0x34, 0x12, // MOV AX,1234h
        0xCF              // IRET
    };
    cpu.write16(0x10u * 4u, 0x7D00);
    cpu.write16(0x10u * 4u + 2u, 0x0000);
    cpu.load(int_program, 0x7C00);
    cpu.load(int_handler, 0x7D00);
    regs.cs = 0; regs.ss = 0; regs.esp = 0x9000; regs.eip = 0x7C00;
    regs.eflags = 0x00000302u; // Reserved bit, IF, and TF.
    cpu.run(16);
    CHECK((cpu.registers().eax & 0xFFFFu) == 0x1234);
    CHECK(cpu.registers().cs == 0);
    CHECK(cpu.registers().eip == 0x7C03);
    CHECK(cpu.registers().esp == 0x9000);
    CHECK((cpu.registers().eflags & (Cpu::Interrupt | Cpu::Trap)) ==
          (Cpu::Interrupt | Cpu::Trap));
    CHECK(cpu.halted());

    // PUSHF / POPF round-trip IF after CLI clears it.
    cpu.reset();
    constexpr std::array<std::uint8_t, 4> pushf_popf_program{
        0x9C, // PUSHF
        0xFA, // CLI
        0x9D, // POPF
        0xF4  // HLT
    };
    cpu.load(pushf_popf_program, 0x7C00);
    regs.cs = 0; regs.ss = 0; regs.esp = 0x9000; regs.eip = 0x7C00;
    regs.eflags = 0x00000202u;
    cpu.run(8);
    CHECK((cpu.registers().eflags & Cpu::Interrupt) != 0);
    CHECK(cpu.registers().esp == 0x9000);
    CHECK(cpu.halted());

    // Primary-master ATA IDENTIFY DEVICE, PIO read/write, and error reporting.
    std::vector<std::uint8_t> disk_image(4u * AtaPioDevice::sector_bytes, 0);
    for (std::size_t i = 0; i < AtaPioDevice::sector_bytes; ++i)
        disk_image[AtaPioDevice::sector_bytes + i] = static_cast<std::uint8_t>(i);
    AtaPioDevice ata(std::move(disk_image));
    CHECK(ata.status() == 0x50);

    ata.write_port(0x1F6, 8, 0xE0); // Master, LBA mode.
    ata.write_port(0x1F7, 8, 0xEC); // IDENTIFY DEVICE.
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) != 0);
    std::uint16_t identify_word0 = 0;
    std::uint16_t identify_model_first_word = 0;
    std::uint16_t identify_sector_count = 0;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto word = static_cast<std::uint16_t>(ata.read_port(0x1F0, 16));
        if (i == 0) identify_word0 = word;
        if (i == 27) identify_model_first_word = word;
        if (i == 60) identify_sector_count = word;
    }
    CHECK(identify_word0 == 0x0040);
    CHECK(identify_model_first_word == 0x5850); // "XP", ATA word byte order.
    CHECK(identify_sector_count == 4);
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) == 0);

    // READ SECTORS returns the chosen sector as little-endian 16-bit words.
    ata.write_port(0x1F2, 8, 1);
    ata.write_port(0x1F3, 8, 1); // LBA 1
    ata.write_port(0x1F4, 8, 0);
    ata.write_port(0x1F5, 8, 0);
    ata.write_port(0x1F7, 8, 0x20);
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) != 0);
    CHECK(ata.read_port(0x1F0, 16) == 0x0100);
    for (std::size_t i = 1; i < 256; ++i) (void)ata.read_port(0x1F0, 16);
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) == 0);
    CHECK(ata.read_port(0x1F2, 8) == 0);

    // WRITE SECTORS commits a full 512-byte sector back to the supplied image.
    ata.write_port(0x1F2, 8, 1);
    ata.write_port(0x1F3, 8, 2); // LBA 2
    ata.write_port(0x1F4, 8, 0);
    ata.write_port(0x1F5, 8, 0);
    ata.write_port(0x1F7, 8, 0x30);
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) != 0);
    for (std::uint32_t i = 0; i < 256; ++i)
        ata.write_port(0x1F0, 16, 0xA000u + i);
    CHECK(ata.image()[2u * AtaPioDevice::sector_bytes] == 0x00);
    CHECK(ata.image()[2u * AtaPioDevice::sector_bytes + 1u] == 0xA0);
    CHECK(ata.image()[3u * AtaPioDevice::sector_bytes - 2u] == 0xFF);
    CHECK(ata.image()[3u * AtaPioDevice::sector_bytes - 1u] == 0xA0);
    CHECK((ata.read_port(0x1F7, 8) & 0x08u) == 0);

    // An out-of-range LBA raises IDNF/ERR without exposing a data buffer.
    ata.write_port(0x1F2, 8, 1);
    ata.write_port(0x1F3, 8, 16);
    ata.write_port(0x1F7, 8, 0x20);
    CHECK((ata.read_port(0x1F7, 8) & 0x01u) != 0);
    CHECK(ata.read_port(0x1F1, 8) == 0x10);

    // Software reset is asserted and then cleared through the control port.
    ata.write_port(0x3F6, 8, 0x04);
    CHECK(ata.status() == 0x80);
    ata.write_port(0x3F6, 8, 0x00);
    CHECK(ata.status() == 0x50);
    CHECK(ata.error() == 0);

    // The CPU can issue ATA task-file commands through its port-I/O callbacks.
    Cpu bus_cpu(1024u * 1024u);
    AtaPioDevice attached_ata(std::vector<std::uint8_t>(AtaPioDevice::sector_bytes, 0));
    attached_ata.connect(bus_cpu);
    constexpr std::array<std::uint8_t, 23> ata_bus_program{
        0xBA, 0xF6, 0x01, // MOV DX,1F6h
        0xB0, 0xE0,       // MOV AL,E0h
        0xEE,             // OUT DX,AL
        0xBA, 0xF2, 0x01, // MOV DX,1F2h
        0xB0, 0x01,       // MOV AL,1
        0xEE,             // OUT DX,AL
        0xBA, 0xF7, 0x01, // MOV DX,1F7h
        0xB0, 0xEC,       // MOV AL,ECh
        0xEE,             // OUT DX,AL
        0xBA, 0xF0, 0x01, // MOV DX,1F0h
        0xED,             // IN AX,DX
        0xF4              // HLT
    };
    bus_cpu.load(ata_bus_program, 0x7C00);
    bus_cpu.registers().cs = 0;
    bus_cpu.registers().eip = 0x7C00;
    bus_cpu.run(32);
    CHECK((bus_cpu.registers().eax & 0xFFFFu) == 0x0040);
    CHECK(bus_cpu.halted());

    // The RAM backing store reaches beyond the original 1 MiB limit.
    cpu.write32(2u * 1024u * 1024u, 0xDEADBEEFu);
    CHECK(cpu.read32(2u * 1024u * 1024u) == 0xDEADBEEFu);

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
