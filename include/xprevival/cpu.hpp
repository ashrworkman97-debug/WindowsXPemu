#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xprevival {

class Cpu {
public:
    static constexpr std::size_t default_ram_size = 128u * 1024u * 1024u;

    enum Flag : std::uint16_t {
        Carry = 1u << 0,
        Parity = 1u << 2,
        AuxiliaryCarry = 1u << 4,
        Zero = 1u << 6,
        Sign = 1u << 7,
        Trap = 1u << 8,
        Interrupt = 1u << 9,
        Direction = 1u << 10,
        Overflow = 1u << 11
    };

    struct Registers {
        std::uint16_t ax{}, cx{}, dx{}, bx{};
        std::uint16_t sp{}, bp{}, si{}, di{};
        std::uint16_t cs{}, ds{}, es{}, ss{};
        std::uint16_t ip{};
        std::uint16_t flags{0x0002};
    };

    explicit Cpu(std::size_t ram_bytes = default_ram_size);

    void reset();
    void load(std::span<const std::uint8_t> bytes, std::uint32_t physical_address);
    void set_reset_state();
    void step();
    void run(std::size_t instruction_limit);

    [[nodiscard]] std::size_t ram_size() const noexcept { return memory_.size(); }
    [[nodiscard]] Registers& registers() noexcept { return regs_; }
    [[nodiscard]] const Registers& registers() const noexcept { return regs_; }
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const;
    void write8(std::uint32_t address, std::uint8_t value);
    void write16(std::uint32_t address, std::uint16_t value);
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] std::uint32_t physical(std::uint16_t segment, std::uint16_t offset) const noexcept;
    [[nodiscard]] std::uint8_t fetch8();
    [[nodiscard]] std::uint16_t fetch16();

    std::vector<std::uint8_t> memory_;
    Registers regs_{};
    bool halted_{false};
    std::string last_error_{};
};

} // namespace xprevival
