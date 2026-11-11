#include "xprevival/atapi.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xprevival {
namespace {
constexpr std::uint8_t StatusError = 0x01;
constexpr std::uint8_t StatusDataRequest = 0x08;
constexpr std::uint8_t StatusReady = 0x40;
constexpr std::uint8_t StatusBusy = 0x80;
constexpr std::uint8_t SenseNotReady = 0x02;
constexpr std::uint8_t SenseIllegalRequest = 0x05;
constexpr std::uint8_t SenseMediumError = 0x03;
constexpr std::uint8_t AscNoMedium = 0x3A;
constexpr std::uint8_t AscInvalidCommand = 0x20;
constexpr std::uint8_t AscLbaOutOfRange = 0x21;
constexpr std::uint8_t AscInvalidField = 0x24;
}

AtapiCdromDevice::AtapiCdromDevice(std::vector<std::uint8_t> image,
                                   std::string vendor, std::string product)
    : image_(std::move(image)), vendor_(std::move(vendor)), product_(std::move(product)) {
    if (!image_.empty() && (image_.size() % sector_bytes) != 0)
        throw std::invalid_argument("ATAPI CD image must contain complete 2048-byte sectors");
    reset();
}

std::uint32_t AtapiCdromDevice::width_mask(unsigned width) noexcept {
    if (width == 8) return 0xFFu;
    if (width == 16) return 0xFFFFu;
    if (width == 32) return 0xFFFFFFFFu;
    return 0u;
}

std::uint32_t AtapiCdromDevice::read_be32(
    const std::array<std::uint8_t, packet_bytes>& packet, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(packet[offset]) << 24) |
           (static_cast<std::uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(packet[offset + 2]) << 8) |
           static_cast<std::uint32_t>(packet[offset + 3]);
}

std::uint16_t AtapiCdromDevice::read_be16(
    const std::array<std::uint8_t, packet_bytes>& packet, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(packet[offset]) << 8) |
                                      packet[offset + 1]);
}

void AtapiCdromDevice::reset() noexcept {
    transfer_ = Transfer::None;
    transfer_buffer_.clear();
    transfer_offset_ = 0;
    phase_remaining_ = 0;
    packet_.fill(0);
    packet_offset_ = 0;
    error_ = 0x01; // ATAPI signature: command packet device.
    interrupt_reason_ = 0x01;
    sector_count_ = 0x01;
    lba_mid_ = 0x14;
    lba_high_ = 0xEB;
    drive_head_ = 0xF0;
    status_ = static_cast<std::uint8_t>(StatusReady);
    sense_key_ = image_.empty() ? SenseNotReady : 0;
    additional_sense_code_ = image_.empty() ? AscNoMedium : 0;
}

void AtapiCdromDevice::finish() noexcept {
    transfer_ = Transfer::None;
    transfer_buffer_.clear();
    transfer_offset_ = 0;
    phase_remaining_ = 0;
    interrupt_reason_ = 0x03; // command/data phase complete
    sector_count_ = interrupt_reason_;
    error_ = 0;
    status_ = StatusReady;
}

void AtapiCdromDevice::fail(std::uint8_t sense_key, std::uint8_t asc) noexcept {
    transfer_ = Transfer::None;
    transfer_buffer_.clear();
    transfer_offset_ = 0;
    phase_remaining_ = 0;
    sense_key_ = sense_key;
    additional_sense_code_ = asc;
    error_ = static_cast<std::uint8_t>(sense_key << 4);
    interrupt_reason_ = 0x03;
    sector_count_ = interrupt_reason_;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusError);
}

void AtapiCdromDevice::update_data_phase() noexcept {
    const auto remaining = transfer_buffer_.size() - transfer_offset_;
    phase_remaining_ = std::min<std::size_t>(remaining, 0xFFFEu);
    lba_mid_ = static_cast<std::uint8_t>(phase_remaining_ & 0xFFu);
    lba_high_ = static_cast<std::uint8_t>((phase_remaining_ >> 8) & 0xFFu);
    interrupt_reason_ = 0x02; // device-to-host data phase
    sector_count_ = interrupt_reason_;
    status_ = phase_remaining_ == 0 ? StatusReady
                                    : static_cast<std::uint8_t>(StatusReady | StatusDataRequest);
}

void AtapiCdromDevice::begin_data(std::vector<std::uint8_t> data) {
    transfer_buffer_ = std::move(data);
    transfer_offset_ = 0;
    transfer_ = Transfer::DataIn;
    if (transfer_buffer_.empty()) {
        finish();
        return;
    }
    update_data_phase();
}

void AtapiCdromDevice::begin_packet() noexcept {
    packet_.fill(0);
    packet_offset_ = 0;
    transfer_buffer_.clear();
    transfer_offset_ = 0;
    phase_remaining_ = 0;
    transfer_ = Transfer::Packet;
    interrupt_reason_ = 0x01; // command packet phase
    sector_count_ = interrupt_reason_;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusDataRequest);
}

void AtapiCdromDevice::prepare_identify() {
    std::vector<std::uint8_t> identify(512, 0);
    const auto put_word = [&identify](std::size_t index, std::uint16_t value) {
        identify[index * 2] = static_cast<std::uint8_t>(value);
        identify[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put_string = [&identify](std::size_t first_word, std::size_t words,
                                        const std::string& value) {
        for (std::size_t i = 0; i < words * 2; i += 2) {
            const auto a = i < value.size() ? static_cast<std::uint8_t>(value[i]) : static_cast<std::uint8_t>(' ');
            const auto b = i + 1 < value.size() ? static_cast<std::uint8_t>(value[i + 1]) : static_cast<std::uint8_t>(' ');
            identify[first_word * 2 + i] = b;
            identify[first_word * 2 + i + 1] = a;
        }
    };
    put_word(0, 0x8580); // ATAPI CD-ROM, removable, 12-byte packet.
    put_word(49, 1u << 9); // LBA supported.
    put_word(53, 1);
    put_word(80, 1u << 2); // ATA/ATAPI command set version support.
    put_string(10, 10, "XPREVIVALATAPI000001");
    put_string(23, 4, "0.9");
    put_string(27, 20, vendor_ + " " + product_);
    transfer_buffer_ = std::move(identify);
    transfer_offset_ = 0;
    transfer_ = Transfer::Identify;
    phase_remaining_ = 512;
    lba_mid_ = 0;
    lba_high_ = 2;
    interrupt_reason_ = 0x02;
    sector_count_ = interrupt_reason_;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusDataRequest);
}

void AtapiCdromDevice::execute_command(std::uint8_t command) {
    status_ = StatusBusy;
    error_ = 0;
    switch (command) {
        case 0xA1: // IDENTIFY PACKET DEVICE
            prepare_identify();
            return;
        case 0xA0: // PACKET
            begin_packet();
            return;
        default:
            fail(SenseIllegalRequest, AscInvalidCommand);
            return;
    }
}

void AtapiCdromDevice::receive_packet_byte(std::uint8_t value) {
    if (packet_offset_ >= packet_.size()) return;
    packet_[packet_offset_++] = value;
    if (packet_offset_ == packet_.size()) {
        transfer_ = Transfer::None;
        status_ = StatusBusy;
        execute_packet();
    }
}

void AtapiCdromDevice::execute_packet() {
    const auto opcode = packet_[0];
    switch (opcode) {
        case 0x00: // TEST UNIT READY
            if (image_.empty()) fail(SenseNotReady, AscNoMedium);
            else finish();
            return;

        case 0x03: { // REQUEST SENSE
            std::vector<std::uint8_t> response(18, 0);
            response[0] = 0x70;
            response[2] = sense_key_;
            response[7] = 10;
            response[12] = additional_sense_code_;
            const auto allocation = packet_[4] == 0 ? response.size() : packet_[4];
            response.resize(std::min<std::size_t>(response.size(), allocation));
            sense_key_ = 0;
            additional_sense_code_ = 0;
            if (image_.empty()) { sense_key_ = SenseNotReady; additional_sense_code_ = AscNoMedium; }
            begin_data(std::move(response));
            return;
        }

        case 0x12: { // INQUIRY
            std::vector<std::uint8_t> response(36, 0);
            response[0] = 0x05; // CD/DVD device.
            response[1] = 0x80; // Removable medium.
            response[2] = 0x05;
            response[3] = 0x02;
            response[4] = 31;
            const auto copy_field = [&response](std::size_t offset, std::size_t length,
                                                const std::string& source) {
                for (std::size_t i = 0; i < length; ++i)
                    response[offset + i] = i < source.size() ? static_cast<std::uint8_t>(source[i]) : static_cast<std::uint8_t>(' ');
            };
            copy_field(8, 8, vendor_);
            copy_field(16, 16, product_);
            copy_field(32, 4, "0.9");
            const auto allocation = packet_[4] == 0 ? response.size() : packet_[4];
            response.resize(std::min<std::size_t>(response.size(), allocation));
            begin_data(std::move(response));
            return;
        }

        case 0x25: { // READ CAPACITY (10)
            if (image_.empty()) { fail(SenseNotReady, AscNoMedium); return; }
            std::vector<std::uint8_t> response(8, 0);
            const auto last_lba = static_cast<std::uint32_t>(image_.size() / sector_bytes - 1u);
            response[0] = static_cast<std::uint8_t>(last_lba >> 24);
            response[1] = static_cast<std::uint8_t>(last_lba >> 16);
            response[2] = static_cast<std::uint8_t>(last_lba >> 8);
            response[3] = static_cast<std::uint8_t>(last_lba);
            response[4] = 0;
            response[5] = 0;
            response[6] = 0x08;
            response[7] = 0x00; // 2048 bytes per logical block.
            begin_data(std::move(response));
            return;
        }

        case 0x28: { // READ(10)
            if (image_.empty()) { fail(SenseNotReady, AscNoMedium); return; }
            const auto lba = read_be32(packet_, 2);
            const auto blocks = read_be16(packet_, 7);
            if (blocks == 0) { finish(); return; }
            const auto block_count = static_cast<std::size_t>(blocks);
            const auto total_blocks = image_.size() / sector_bytes;
            if (lba >= total_blocks || block_count > total_blocks - lba) {
                fail(SenseMediumError, AscLbaOutOfRange);
                return;
            }
            const auto byte_count = block_count * sector_bytes;
            const auto offset = static_cast<std::size_t>(lba) * sector_bytes;
            std::vector<std::uint8_t> response(byte_count);
            std::copy_n(image_.begin() + offset, byte_count, response.begin());
            begin_data(std::move(response));
            return;
        }

        case 0x43: { // READ TOC/PMA/ATIP, format 0, one data track.
            if (image_.empty()) { fail(SenseNotReady, AscNoMedium); return; }
            std::vector<std::uint8_t> response(20, 0);
            response[0] = 0;
            response[1] = 18; // bytes following the length field
            response[2] = 1;  // first track
            response[3] = 1;  // last track
            response[5] = 0x14;
            response[6] = 1;
            const auto leadout = static_cast<std::uint32_t>(image_.size() / sector_bytes);
            response[12] = 0;
            response[13] = 0x14;
            response[14] = 0xAA;
            response[16] = static_cast<std::uint8_t>(leadout >> 24);
            response[17] = static_cast<std::uint8_t>(leadout >> 16);
            response[18] = static_cast<std::uint8_t>(leadout >> 8);
            response[19] = static_cast<std::uint8_t>(leadout);
            const auto allocation = (static_cast<std::uint16_t>(packet_[7]) << 8) | packet_[8];
            response.resize(std::min<std::size_t>(response.size(), allocation));
            begin_data(std::move(response));
            return;
        }

        case 0x1B: // START STOP UNIT (no mechanical eject is modeled).
        case 0x1E: // PREVENT/ALLOW MEDIUM REMOVAL.
            finish();
            return;

        default:
            fail(SenseIllegalRequest, AscInvalidCommand);
            return;
    }
}

std::uint32_t AtapiCdromDevice::read_data(unsigned width) {
    if (transfer_ != Transfer::Identify && transfer_ != Transfer::DataIn) return width_mask(width);
    const auto count = width / 8u;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < count; ++i) {
        std::uint8_t value = 0;
        if (transfer_offset_ < transfer_buffer_.size() && phase_remaining_ != 0) {
            value = transfer_buffer_[transfer_offset_++];
            --phase_remaining_;
        }
        result |= static_cast<std::uint32_t>(value) << (i * 8u);
    }
    if (phase_remaining_ == 0) {
        if (transfer_ == Transfer::Identify || transfer_offset_ >= transfer_buffer_.size()) finish();
        else update_data_phase();
    }
    return result & width_mask(width);
}

void AtapiCdromDevice::write_data(unsigned width, std::uint32_t value) {
    if (transfer_ != Transfer::Packet) return;
    const auto count = width / 8u;
    for (unsigned i = 0; i < count; ++i)
        receive_packet_byte(static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu));
}

std::uint32_t AtapiCdromDevice::read_port(std::uint16_t port, unsigned width) {
    const auto mask = width_mask(width);
    if (mask == 0) return 0xFFFFFFFFu;
    std::uint8_t value = 0xFF;
    switch (port) {
        case 0x1F0: return read_data(width);
        case 0x1F1: value = error_; break;
        case 0x1F2: value = sector_count_; break;
        case 0x1F3: value = interrupt_reason_; break;
        case 0x1F4: value = lba_mid_; break;
        case 0x1F5: value = lba_high_; break;
        case 0x1F6: value = drive_head_; break;
        case 0x1F7:
        case 0x3F6: value = status_; break;
        default: break;
    }
    return static_cast<std::uint32_t>(value) & mask;
}

void AtapiCdromDevice::write_port(std::uint16_t port, unsigned width, std::uint32_t value) {
    if (width_mask(width) == 0) return;
    const auto byte = static_cast<std::uint8_t>(value & 0xFFu);
    switch (port) {
        case 0x1F0:
            write_data(width, value);
            break;
        case 0x1F1: break; // Features register.
        case 0x1F2: sector_count_ = byte; break;
        case 0x1F4: lba_mid_ = byte; break; // Host maximum byte count, low.
        case 0x1F5: lba_high_ = byte; break; // Host maximum byte count, high.
        case 0x1F6: drive_head_ = byte; break;
        case 0x1F7: execute_command(byte); break;
        case 0x3F6: {
            const bool was_reset = (device_control_ & 0x04u) != 0;
            device_control_ = byte;
            if ((byte & 0x04u) != 0) status_ = StatusBusy;
            else if (was_reset) reset();
            break;
        }
        default: break;
    }
}

} // namespace xprevival
