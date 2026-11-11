#pragma once

#include "xprevival/cpu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xprevival {

// Primary-channel, master-device ATA PIO controller backed by a caller-owned
// raw disk image. This models the task-file/data registers and does not perform
// host file I/O; callers can persist image() when the guest has written sectors.
class AtaPioDevice final {
public:
    static constexpr std::size_t sector_bytes = 512;

    explicit AtaPioDevice(std::vector<std::uint8_t> image,
                          std::string model = "XP Revival Virtual ATA Disk");

    void connect(Cpu& cpu);

    [[nodiscard]] std::uint32_t read_port(std::uint16_t port, unsigned width);
    void write_port(std::uint16_t port, unsigned width, std::uint32_t value);

    [[nodiscard]] const std::vector<std::uint8_t>& image() const noexcept { return image_; }
    [[nodiscard]] std::vector<std::uint8_t>& image() noexcept { return image_; }
    [[nodiscard]] std::uint8_t status() const noexcept { return status_; }
    [[nodiscard]] std::uint8_t error() const noexcept { return error_; }

private:
    enum class Transfer : std::uint8_t { None, Identify, Read, Write };

    static constexpr std::uint8_t StatusError = 0x01;
    static constexpr std::uint8_t StatusDataRequest = 0x08;
    static constexpr std::uint8_t StatusSeekComplete = 0x10;
    static constexpr std::uint8_t StatusReady = 0x40;
    static constexpr std::uint8_t StatusBusy = 0x80;
    static constexpr std::uint8_t ErrorAbort = 0x04;
    static constexpr std::uint8_t ErrorIdNotFound = 0x10;

    [[nodiscard]] std::uint32_t width_mask(unsigned width) const noexcept;
    [[nodiscard]] std::uint32_t current_lba() const noexcept;
    void update_taskfile_lba() noexcept;
    void execute_command(std::uint8_t command);
    void fail(std::uint8_t error_code) noexcept;
    void finish_transfer() noexcept;
    void begin_read();
    void begin_write();
    void prepare_identify();
    void advance_read_buffer();
    void commit_write_buffer();
    [[nodiscard]] std::uint32_t read_data(unsigned width);
    void write_data(unsigned width, std::uint32_t value);
    void software_reset() noexcept;

    std::vector<std::uint8_t> image_;
    std::string model_;
    std::array<std::uint8_t, sector_bytes> buffer_{};
    std::size_t buffer_offset_{};
    std::uint32_t current_lba_{};
    std::uint32_t sectors_remaining_{};
    std::uint8_t error_{};
    std::uint8_t features_{};
    std::uint8_t sector_count_{1};
    std::uint8_t lba_low_{};
    std::uint8_t lba_mid_{};
    std::uint8_t lba_high_{};
    std::uint8_t drive_head_{0xE0};
    std::uint8_t status_{StatusReady | StatusSeekComplete};
    std::uint8_t device_control_{};
    Transfer transfer_{Transfer::None};
};

} // namespace xprevival
