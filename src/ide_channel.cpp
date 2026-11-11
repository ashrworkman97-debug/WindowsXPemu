#include "xprevival/ide_channel.hpp"

namespace xprevival {

void IdePioChannel::connect(Cpu& cpu) {
    cpu.set_io_callbacks(
        [this](std::uint16_t port, unsigned width) { return read_port(port, width); },
        [this](std::uint16_t port, unsigned width, std::uint32_t value) { write_port(port, width, value); });
}

std::uint32_t IdePioChannel::read_port(std::uint16_t port, unsigned width) {
    return slave_selected_ ? slave_.read_port(port, width) : master_.read_port(port, width);
}

void IdePioChannel::write_port(std::uint16_t port, unsigned width, std::uint32_t value) {
    if (port == 0x1F6) slave_selected_ = (value & 0x10u) != 0;
    if (slave_selected_) slave_.write_port(port, width, value);
    else master_.write_port(port, width, value);
}

} // namespace xprevival
