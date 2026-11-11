#pragma once

#include "xprevival/ata_pio.hpp"
#include "xprevival/atapi.hpp"

#include <cstdint>

namespace xprevival {

// Primary IDE channel multiplexer: ATA hard disk as master, ATAPI CD-ROM as slave.
class IdePioChannel final {
public:
    IdePioChannel(AtaPioDevice& master, AtapiCdromDevice& slave) noexcept
        : master_(master), slave_(slave) {}

    void connect(Cpu& cpu);
    [[nodiscard]] std::uint32_t read_port(std::uint16_t port, unsigned width);
    void write_port(std::uint16_t port, unsigned width, std::uint32_t value);
    [[nodiscard]] bool slave_selected() const noexcept { return slave_selected_; }

private:
    AtaPioDevice& master_;
    AtapiCdromDevice& slave_;
    bool slave_selected_{false};
};

} // namespace xprevival
