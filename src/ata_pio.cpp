#include "xprevival/ata_pio.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xprevival {

AtaPioDevice::AtaPioDevice(std::vector<std::uint8_t> image, std::string model)
    : image_(std::move(image)), model_(std::move(model)) {
    if (image_.empty() || (image_.size() % sector_bytes) != 0)
        throw std::invalid_argument("ATA disk image must contain one or more complete 512-byte sectors");
    const auto sectors = image_.size() / sector_bytes;
    if (sectors > 0x0FFFFFFFu)
        throw std::invalid_argument("ATA disk image exceeds the 28-bit LBA capacity supported by this device");
    software_reset();
}

void AtaPioDevice::connect(Cpu& cpu) {
    cpu.set_io_callbacks(
        [this](std::uint16_t port, unsigned width) { return read_port(port, width); },
        [this](std::uint16_t port, unsigned width, std::uint32_t value) { write_port(port, width, value); });
}

std::uint32_t AtaPioDevice::width_mask(unsigned width) const noexcept {
    if (width == 8) return 0xFFu;
    if (width == 16) return 0xFFFFu;
    if (width == 32) return 0xFFFFFFFFu;
    return 0u;
}

std::uint32_t AtaPioDevice::current_lba() const noexcept {
    if ((drive_head_ & 0x40u) != 0) {
        return (static_cast<std::uint32_t>(drive_head_ & 0x0Fu) << 24) |
               (static_cast<std::uint32_t>(lba_high_) << 16) |
               (static_cast<std::uint32_t>(lba_mid_) << 8) |
               static_cast<std::uint32_t>(lba_low_);
    }

    // The legacy CHS translation uses the common 16-head / 63-sector geometry.
    const auto sector = static_cast<std::uint32_t>(lba_low_ & 0x3Fu);
    if (sector == 0 || (drive_head_ & 0x0Fu) >= 16u)
        return std::numeric_limits<std::uint32_t>::max();
    const auto cylinder = (static_cast<std::uint32_t>(lba_high_) << 8) |
                          static_cast<std::uint32_t>(lba_mid_);
    const auto head = static_cast<std::uint32_t>(drive_head_ & 0x0Fu);
    return ((cylinder * 16u + head) * 63u) + (sector - 1u);
}

void AtaPioDevice::update_taskfile_lba() noexcept {
    if ((drive_head_ & 0x40u) != 0) {
        lba_low_ = static_cast<std::uint8_t>(current_lba_ & 0xFFu);
        lba_mid_ = static_cast<std::uint8_t>((current_lba_ >> 8) & 0xFFu);
        lba_high_ = static_cast<std::uint8_t>((current_lba_ >> 16) & 0xFFu);
        drive_head_ = static_cast<std::uint8_t>((drive_head_ & 0xF0u) |
                                                ((current_lba_ >> 24) & 0x0Fu));
    } else {
        constexpr std::uint32_t sectors_per_cylinder = 16u * 63u;
        const auto cylinder = current_lba_ / sectors_per_cylinder;
        const auto within_cylinder = current_lba_ % sectors_per_cylinder;
        const auto head = within_cylinder / 63u;
        const auto sector = (within_cylinder % 63u) + 1u;
        lba_low_ = static_cast<std::uint8_t>(sector);
        lba_mid_ = static_cast<std::uint8_t>(cylinder & 0xFFu);
        lba_high_ = static_cast<std::uint8_t>((cylinder >> 8) & 0xFFu);
        drive_head_ = static_cast<std::uint8_t>((drive_head_ & 0xF0u) | (head & 0x0Fu));
    }
}

void AtaPioDevice::fail(std::uint8_t error_code) noexcept {
    transfer_ = Transfer::None;
    sectors_remaining_ = 0;
    buffer_offset_ = 0;
    error_ = error_code;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusError);
}

void AtaPioDevice::finish_transfer() noexcept {
    transfer_ = Transfer::None;
    sectors_remaining_ = 0;
    buffer_offset_ = 0;
    error_ = 0;
    sector_count_ = 0;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete);
}

void AtaPioDevice::prepare_identify() {
    buffer_.fill(0);
    const auto put_word = [this](std::size_t index, std::uint16_t value) {
        buffer_[index * 2] = static_cast<std::uint8_t>(value & 0xFFu);
        buffer_[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put_string = [this](std::size_t first_word, std::size_t word_count,
                                   const std::string& value) {
        const auto byte_offset = first_word * 2;
        for (std::size_t i = 0; i < word_count * 2; i += 2) {
            const auto first = i < value.size() ? static_cast<std::uint8_t>(value[i]) : static_cast<std::uint8_t>(' ');
            const auto second = i + 1 < value.size() ? static_cast<std::uint8_t>(value[i + 1]) : static_cast<std::uint8_t>(' ');
            // ATA IDENTIFY strings store the first character in the high byte of each word.
            buffer_[byte_offset + i] = second;
            buffer_[byte_offset + i + 1] = first;
        }
    };

    const auto sectors = static_cast<std::uint32_t>(image_.size() / sector_bytes);
    const auto chs_capacity = std::min<std::uint32_t>(sectors, 16383u * 16u * 63u);
    put_word(0, 0x0040);       // Fixed disk; non-removable.
    put_word(1, 16383);        // Legacy translated geometry.
    put_word(3, 16);
    put_word(6, 63);
    put_word(47, 0x8001);      // Multiple-sector transfers: one sector per block.
    put_word(49, 1u << 9);     // LBA supported.
    put_word(53, 0x0001);      // Words 54-58 are valid.
    put_word(54, 16383);
    put_word(55, 16);
    put_word(56, 63);
    put_word(57, static_cast<std::uint16_t>(chs_capacity & 0xFFFFu));
    put_word(58, static_cast<std::uint16_t>(chs_capacity >> 16));
    put_word(60, static_cast<std::uint16_t>(sectors & 0xFFFFu));
    put_word(61, static_cast<std::uint16_t>((sectors >> 16) & 0xFFFFu));
    put_string(10, 10, "XPREVIVAL000000000001");
    put_string(23, 4, "0.8");
    put_string(27, 20, model_);

    buffer_offset_ = 0;
    sectors_remaining_ = 1;
    transfer_ = Transfer::Identify;
    error_ = 0;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusDataRequest);
}

void AtaPioDevice::begin_read() {
    const auto lba = current_lba();
    const auto count = sector_count_ == 0 ? 256u : static_cast<std::uint32_t>(sector_count_);
    const auto total_sectors = static_cast<std::uint32_t>(image_.size() / sector_bytes);
    if (lba == std::numeric_limits<std::uint32_t>::max() ||
        lba >= total_sectors || count > total_sectors - lba) {
        fail(ErrorIdNotFound);
        return;
    }

    current_lba_ = lba;
    sectors_remaining_ = count;
    buffer_offset_ = 0;
    transfer_ = Transfer::Read;
    std::copy_n(image_.begin() + static_cast<std::size_t>(current_lba_) * sector_bytes,
                sector_bytes, buffer_.begin());
    error_ = 0;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusDataRequest);
}

void AtaPioDevice::begin_write() {
    const auto lba = current_lba();
    const auto count = sector_count_ == 0 ? 256u : static_cast<std::uint32_t>(sector_count_);
    const auto total_sectors = static_cast<std::uint32_t>(image_.size() / sector_bytes);
    if (lba == std::numeric_limits<std::uint32_t>::max() ||
        lba >= total_sectors || count > total_sectors - lba) {
        fail(ErrorIdNotFound);
        return;
    }

    current_lba_ = lba;
    sectors_remaining_ = count;
    buffer_offset_ = 0;
    buffer_.fill(0);
    transfer_ = Transfer::Write;
    error_ = 0;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusDataRequest);
}

void AtaPioDevice::execute_command(std::uint8_t command) {
    status_ = StatusBusy;
    error_ = 0;
    transfer_ = Transfer::None;
    buffer_offset_ = 0;
    sectors_remaining_ = 0;

    if ((drive_head_ & 0x10u) != 0) {
        fail(ErrorAbort); // This device implements the primary master only.
        return;
    }

    switch (command) {
        case 0xEC: // IDENTIFY DEVICE
            prepare_identify();
            return;
        case 0x20: // READ SECTORS
            begin_read();
            return;
        case 0x30: // WRITE SECTORS
            begin_write();
            return;
        case 0xE7: // FLUSH CACHE (writes are immediately reflected in image()).
            finish_transfer();
            return;
        case 0x90: // EXECUTE DEVICE DIAGNOSTIC
            error_ = 0x01;
            status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete);
            return;
        default:
            fail(ErrorAbort);
            return;
    }
}

void AtaPioDevice::advance_read_buffer() {
    if (transfer_ == Transfer::Identify) {
        finish_transfer();
        return;
    }
    if (transfer_ != Transfer::Read) return;

    if (sectors_remaining_ != 0) --sectors_remaining_;
    sector_count_ = static_cast<std::uint8_t>(sectors_remaining_ & 0xFFu);
    if (sectors_remaining_ == 0) {
        finish_transfer();
        return;
    }

    ++current_lba_;
    update_taskfile_lba();
    buffer_offset_ = 0;
    std::copy_n(image_.begin() + static_cast<std::size_t>(current_lba_) * sector_bytes,
                sector_bytes, buffer_.begin());
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusDataRequest);
}

void AtaPioDevice::commit_write_buffer() {
    if (transfer_ != Transfer::Write) return;
    const auto disk_offset = static_cast<std::size_t>(current_lba_) * sector_bytes;
    if (disk_offset > image_.size() || sector_bytes > image_.size() - disk_offset) {
        fail(ErrorIdNotFound);
        return;
    }

    std::copy(buffer_.begin(), buffer_.end(), image_.begin() + disk_offset);
    if (sectors_remaining_ != 0) --sectors_remaining_;
    sector_count_ = static_cast<std::uint8_t>(sectors_remaining_ & 0xFFu);
    if (sectors_remaining_ == 0) {
        finish_transfer();
        return;
    }

    ++current_lba_;
    update_taskfile_lba();
    buffer_offset_ = 0;
    buffer_.fill(0);
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete | StatusDataRequest);
}

std::uint32_t AtaPioDevice::read_data(unsigned width) {
    const auto mask = width_mask(width);
    if (mask == 0) return 0xFFFFFFFFu;
    const auto byte_count = width / 8u;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < byte_count; ++i) {
        std::uint8_t value = 0xFF;
        if ((transfer_ == Transfer::Identify || transfer_ == Transfer::Read) &&
            buffer_offset_ < sector_bytes) {
            value = buffer_[buffer_offset_++];
            if (buffer_offset_ == sector_bytes) advance_read_buffer();
        }
        result |= static_cast<std::uint32_t>(value) << (i * 8u);
    }
    return result & mask;
}

void AtaPioDevice::write_data(unsigned width, std::uint32_t value) {
    if (width_mask(width) == 0 || transfer_ != Transfer::Write) return;
    const auto byte_count = width / 8u;
    for (unsigned i = 0; i < byte_count; ++i) {
        if (transfer_ != Transfer::Write || buffer_offset_ >= sector_bytes) break;
        buffer_[buffer_offset_++] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
        if (buffer_offset_ == sector_bytes) commit_write_buffer();
    }
}

std::uint32_t AtaPioDevice::read_port(std::uint16_t port, unsigned width) {
    const auto mask = width_mask(width);
    if (mask == 0) return 0xFFFFFFFFu;
    std::uint8_t value = 0xFF;
    switch (port) {
        case 0x1F0: return read_data(width);
        case 0x1F1: value = error_; break;
        case 0x1F2: value = sector_count_; break;
        case 0x1F3: value = lba_low_; break;
        case 0x1F4: value = lba_mid_; break;
        case 0x1F5: value = lba_high_; break;
        case 0x1F6: value = drive_head_; break;
        case 0x1F7:
        case 0x3F6: value = status_; break; // Status read; no IRQ line is modeled yet.
        default: break;
    }
    return static_cast<std::uint32_t>(value) & mask;
}

void AtaPioDevice::write_port(std::uint16_t port, unsigned width, std::uint32_t value) {
    if (width_mask(width) == 0) return;
    const auto byte = static_cast<std::uint8_t>(value & 0xFFu);
    switch (port) {
        case 0x1F0:
            write_data(width, value);
            break;
        case 0x1F1: features_ = byte; break;
        case 0x1F2: sector_count_ = byte; break;
        case 0x1F3: lba_low_ = byte; break;
        case 0x1F4: lba_mid_ = byte; break;
        case 0x1F5: lba_high_ = byte; break;
        case 0x1F6: drive_head_ = byte; break;
        case 0x1F7: execute_command(byte); break;
        case 0x3F6: {
            const bool was_reset = (device_control_ & 0x04u) != 0;
            device_control_ = byte;
            if ((byte & 0x04u) != 0) {
                transfer_ = Transfer::None;
                sectors_remaining_ = 0;
                buffer_offset_ = 0;
                status_ = StatusBusy;
            } else if (was_reset) {
                software_reset();
            }
            break;
        }
        default: break;
    }
}

void AtaPioDevice::software_reset() noexcept {
    transfer_ = Transfer::None;
    buffer_offset_ = 0;
    sectors_remaining_ = 0;
    error_ = 0;
    features_ = 0;
    sector_count_ = 1;
    lba_low_ = 0;
    lba_mid_ = 0;
    lba_high_ = 0;
    drive_head_ = 0xE0;
    status_ = static_cast<std::uint8_t>(StatusReady | StatusSeekComplete);
}

} // namespace xprevival
