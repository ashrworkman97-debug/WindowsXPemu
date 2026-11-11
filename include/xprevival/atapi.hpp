#pragma once

#include "xprevival/cpu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xprevival {

// ATAPI PACKET CD-ROM target. The host supplies a raw 2048-byte-sector ISO
// image; all guest access occurs through the primary IDE task-file registers.
class AtapiCdromDevice final {
public:
    static constexpr std::size_t sector_bytes = 2048;
    static constexpr std::size_t packet_bytes = 12;

    explicit AtapiCdromDevice(std::vector<std::uint8_t> image = {},
                              std::string vendor = "XPREVIVAL",
                              std::string product = "VIRTUAL CD-ROM");

    [[nodiscard]] std::uint32_t read_port(std::uint16_t port, unsigned width);
    void write_port(std::uint16_t port, unsigned width, std::uint32_t value);

    [[nodiscard]] const std::vector<std::uint8_t>& image() const noexcept { return image_; }
    [[nodiscard]] bool has_media() const noexcept { return !image_.empty(); }
    [[nodiscard]] std::uint8_t status() const noexcept { return status_; }
    [[nodiscard]] std::uint8_t error() const noexcept { return error_; }

private:
    enum class Transfer : std::uint8_t { None, Identify, Packet, DataIn };

    void reset() noexcept;
    void execute_command(std::uint8_t command);
    void begin_packet() noexcept;
    void prepare_identify();
    void receive_packet_byte(std::uint8_t value);
    void execute_packet();
    void begin_data(std::vector<std::uint8_t> data);
    void update_data_phase() noexcept;
    void finish() noexcept;
    void fail(std::uint8_t sense_key, std::uint8_t asc) noexcept;
    [[nodiscard]] std::uint32_t read_data(unsigned width);
    void write_data(unsigned width, std::uint32_t value);
    [[nodiscard]] static std::uint32_t width_mask(unsigned width) noexcept;
    [[nodiscard]] static std::uint32_t read_be32(const std::array<std::uint8_t, packet_bytes>& packet,
                                                 std::size_t offset) noexcept;
    [[nodiscard]] static std::uint16_t read_be16(const std::array<std::uint8_t, packet_bytes>& packet,
                                                 std::size_t offset) noexcept;

    std::vector<std::uint8_t> image_;
    std::string vendor_;
    std::string product_;
    std::array<std::uint8_t, packet_bytes> packet_{};
    std::vector<std::uint8_t> transfer_buffer_;
    std::size_t transfer_offset_{};
    std::size_t phase_remaining_{};
    std::uint8_t error_{};
    std::uint8_t interrupt_reason_{};
    std::uint8_t sector_count_{};
    std::uint8_t lba_mid_{};
    std::uint8_t lba_high_{};
    std::uint8_t drive_head_{0xF0};
    std::uint8_t status_{0x50};
    std::uint8_t device_control_{};
    std::uint8_t sense_key_{};
    std::uint8_t additional_sense_code_{};
    std::size_t packet_offset_{};
    Transfer transfer_{Transfer::None};
};

} // namespace xprevival
