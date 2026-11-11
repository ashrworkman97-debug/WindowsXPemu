#include "xprevival/atapi.hpp"
#include "xprevival/ata_pio.hpp"
#include "xprevival/cpu.hpp"
#include "xprevival/ide_channel.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

void send_packet(xprevival::IdePioChannel& channel,
                 const std::array<std::uint8_t, 12>& packet) {
    channel.write_port(0x1F7, 8, 0xA0); // PACKET
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) != 0);
    for (std::size_t i = 0; i < packet.size(); i += 2) {
        const auto word = static_cast<std::uint16_t>(packet[i] |
                         (static_cast<std::uint16_t>(packet[i + 1]) << 8));
        channel.write_port(0x1F0, 16, word);
    }
}

std::vector<std::uint8_t> read_bytes(xprevival::IdePioChannel& channel, std::size_t count) {
    std::vector<std::uint8_t> result(count);
    for (std::size_t i = 0; i < count; i += 2) {
        const auto word = static_cast<std::uint16_t>(channel.read_port(0x1F0, 16));
        result[i] = static_cast<std::uint8_t>(word);
        if (i + 1 < count) result[i + 1] = static_cast<std::uint8_t>(word >> 8);
    }
    return result;
}
}

int main() {
    using namespace xprevival;
    constexpr std::size_t sectors = 3;
    std::vector<std::uint8_t> iso(sectors * AtapiCdromDevice::sector_bytes, 0);
    for (std::size_t i = 0; i < AtapiCdromDevice::sector_bytes; ++i) {
        iso[AtapiCdromDevice::sector_bytes + i] = static_cast<std::uint8_t>((i * 13u + 7u) & 0xFFu);
    }

    AtapiCdromDevice cdrom(std::move(iso));
    AtaPioDevice disk(std::vector<std::uint8_t>(4u * AtaPioDevice::sector_bytes, 0));
    IdePioChannel channel(disk, cdrom);
    Cpu cpu(4u * 1024u * 1024u);
    channel.connect(cpu);

    // Device-select bit 4 routes the channel to the primary-slave CD-ROM.
    channel.write_port(0x1F6, 8, 0xF0);
    CHECK(channel.slave_selected());
    CHECK(cdrom.has_media());

    // IDENTIFY PACKET DEVICE returns the ATAPI signature and a 512-byte page.
    channel.write_port(0x1F7, 8, 0xA1);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) != 0);
    CHECK(channel.read_port(0x1F0, 16) == 0x8580);
    auto identify_tail = read_bytes(channel, 510);
    CHECK(identify_tail.size() == 510);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) == 0);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) == 0); // IDENTIFY transfer is complete.

    // INQUIRY identifies the target as a removable CD-ROM device.
    constexpr std::array<std::uint8_t, 12> inquiry{
        0x12, 0x00, 0x00, 0x00, 36, 0x00, 0, 0, 0, 0, 0, 0
    };
    send_packet(channel, inquiry);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) != 0);
    const auto inquiry_data = read_bytes(channel, 36);
    CHECK(inquiry_data[0] == 0x05);
    CHECK((inquiry_data[1] & 0x80u) != 0);
    CHECK(std::string(inquiry_data.begin() + 8, inquiry_data.begin() + 16) == "XPREVIVA");
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) == 0);

    // READ CAPACITY reports three 2048-byte logical blocks.
    constexpr std::array<std::uint8_t, 12> capacity{
        0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    send_packet(channel, capacity);
    const auto capacity_data = read_bytes(channel, 8);
    CHECK(capacity_data[3] == 2);
    CHECK(capacity_data[4] == 0);
    CHECK(capacity_data[5] == 0);
    CHECK(capacity_data[6] == 0x08);
    CHECK(capacity_data[7] == 0x00);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) == 0);

    // READ(10) fetches block 1 from the supplied ISO without exposing a host pointer.
    constexpr std::array<std::uint8_t, 12> read10{
        0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0
    };
    send_packet(channel, read10);
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) != 0);
    const auto block = read_bytes(channel, AtapiCdromDevice::sector_bytes);
    CHECK(block.front() == 7);
    CHECK(block[1] == 20);
    CHECK(block[100] == static_cast<std::uint8_t>((100u * 13u + 7u) & 0xFFu));
    CHECK(block.back() == static_cast<std::uint8_t>((2047u * 13u + 7u) & 0xFFu));
    CHECK((channel.read_port(0x1F7, 8) & 0x08u) == 0);

    // Out-of-range READ(10) reports a medium error; REQUEST SENSE returns it.
    constexpr std::array<std::uint8_t, 12> invalid_read{
        0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0, 0, 0
    };
    send_packet(channel, invalid_read);
    CHECK((channel.read_port(0x1F7, 8) & 0x01u) != 0);
    constexpr std::array<std::uint8_t, 12> request_sense{
        0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0
    };
    send_packet(channel, request_sense);
    const auto sense = read_bytes(channel, 18);
    CHECK(sense[2] == 0x03);
    CHECK(sense[12] == 0x21);
    CHECK((channel.read_port(0x1F7, 8) & 0x01u) == 0);

    // Switching device select back to master restores the ATA hard disk.
    channel.write_port(0x1F6, 8, 0xE0);
    CHECK(!channel.slave_selected());
    channel.write_port(0x1F7, 8, 0xEC);
    CHECK(channel.read_port(0x1F0, 16) == 0x0040);

    // The CPU's real IN/OUT instructions use the same IDE channel multiplexer.
    constexpr std::array<std::uint8_t, 17> cpu_program{
        0xBA, 0xF6, 0x01, // MOV DX,1F6h
        0xB0, 0xF0,       // MOV AL,F0h (select CD slave)
        0xEE,             // OUT DX,AL
        0xBA, 0xF7, 0x01, // MOV DX,1F7h
        0xB0, 0xA1,       // MOV AL,A1h (IDENTIFY PACKET)
        0xEE,             // OUT DX,AL
        0xBA, 0xF0, 0x01, // MOV DX,1F0h
        0xED,             // IN AX,DX
        0xF4              // HLT
    };
    cpu.load(cpu_program, 0x7C00);
    cpu.registers().cs = 0;
    cpu.registers().eip = 0x7C00;
    cpu.run(32);
    CHECK((cpu.registers().eax & 0xFFFFu) == 0x8580u);
    CHECK(cpu.halted());
    CHECK(cpu.last_error().empty());

    // An ATAPI device with no mounted image correctly reports no medium.
    AtapiCdromDevice empty_cdrom;
    empty_cdrom.write_port(0x1F7, 8, 0xA0);
    constexpr std::array<std::uint8_t, 12> tur{0,0,0,0,0,0,0,0,0,0,0,0};
    for (std::size_t i = 0; i < tur.size(); i += 2)
        empty_cdrom.write_port(0x1F0, 16, static_cast<std::uint16_t>(tur[i] | (tur[i + 1] << 8)));
    CHECK((empty_cdrom.status() & 0x01u) != 0);
    CHECK((empty_cdrom.error() >> 4) == 0x02u);

    if (failures != 0) {
        std::cerr << failures << " ATAPI test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "ATAPI CD-ROM tests passed.\n";
    return EXIT_SUCCESS;
}
