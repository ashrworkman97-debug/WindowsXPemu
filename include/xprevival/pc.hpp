#pragma once

#include "xprevival/ata_pio.hpp"
#include "xprevival/atapi.hpp"
#include "xprevival/ide_channel.hpp"
#include "xprevival/cpu.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xprevival {

// A deliberately small, original PC assembly: CPU + primary-master IDE disk
// + the conventional color-text buffer at physical address 0xB8000.
class Pc final {
public:
    explicit Pc(std::vector<std::uint8_t> disk_image,
                std::size_t ram_bytes = Cpu::default_ram_size);
    Pc(std::vector<std::uint8_t> disk_image, std::vector<std::uint8_t> cdrom_image,
       std::size_t ram_bytes = Cpu::default_ram_size);

    // Load sector 0 at 0000:7C00 and establish the conventional BIOS handoff
    // register state. This is a direct boot-sector handoff, not a BIOS.
    void boot();

    // Execute up to instruction_limit guest instructions. REP strings are
    // handled by the CPU core as a single decoded instruction.
    [[nodiscard]] std::size_t run(std::size_t instruction_limit);

    [[nodiscard]] std::string text_screen() const;

    [[nodiscard]] Cpu& cpu() noexcept { return cpu_; }
    [[nodiscard]] const Cpu& cpu() const noexcept { return cpu_; }
    [[nodiscard]] AtaPioDevice& disk() noexcept { return ata_; }
    [[nodiscard]] AtapiCdromDevice& cdrom() noexcept { return atapi_; }
    [[nodiscard]] const AtapiCdromDevice& cdrom() const noexcept { return atapi_; }
    [[nodiscard]] const AtaPioDevice& disk() const noexcept { return ata_; }
    [[nodiscard]] bool booted() const noexcept { return booted_; }

private:
    [[nodiscard]] bool handle_bios_interrupt(Cpu& cpu, std::uint8_t vector);
    void set_interrupt_carry(Cpu& cpu, bool carry) const;
    void write_teletype_character(Cpu& cpu, std::uint8_t character);
    Cpu cpu_;
    AtaPioDevice ata_;
    AtapiCdromDevice atapi_;
    IdePioChannel ide_channel_;
    std::uint16_t vga_cursor_{};
    std::uint8_t video_mode_{3};
    bool booted_{false};
};

// Builds a small raw IDE disk containing an original 16-bit boot sector.
// The sector writes text through VGA text memory and executes ATA IDENTIFY
// through the modeled primary IDE ports. It is a hardware-path smoke demo.
[[nodiscard]] std::vector<std::uint8_t> make_demo_disk_image();

} // namespace xprevival
