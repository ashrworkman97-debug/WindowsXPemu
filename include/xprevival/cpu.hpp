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

    enum Flag : std::uint32_t {
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

    // 386-width general-purpose registers are available even while the CPU is
    // executing with a 16-bit real-mode code segment. Segment selectors remain
    // 16-bit; protected-mode descriptor caches are a later milestone.
    struct Registers {
        std::uint32_t eax{}, ecx{}, edx{}, ebx{};
        std::uint32_t esp{}, ebp{}, esi{}, edi{};
        std::uint16_t cs{}, ds{}, es{}, ss{}, fs{}, gs{};
        std::uint32_t eip{};
        std::uint32_t eflags{0x00000002};
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
    [[nodiscard]] std::uint32_t read32(std::uint32_t address) const;
    void write8(std::uint32_t address, std::uint8_t value);
    void write16(std::uint32_t address, std::uint16_t value);
    void write32(std::uint32_t address, std::uint32_t value);
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] std::uint32_t physical(std::uint16_t segment, std::uint16_t offset) const noexcept;
    [[nodiscard]] std::uint8_t fetch8();
    [[nodiscard]] std::uint16_t fetch16();
    [[nodiscard]] std::uint32_t fetch32();
    [[nodiscard]] std::uint32_t read_general(std::uint8_t index, bool operand32) const noexcept;
    void write_general(std::uint8_t index, bool operand32, std::uint32_t value) noexcept;
    [[nodiscard]] bool decode_register_modrm(std::uint8_t& reg, std::uint8_t& rm);
    [[nodiscard]] std::uint32_t add(std::uint32_t lhs, std::uint32_t rhs, bool operand32) noexcept;
    [[nodiscard]] std::uint32_t subtract(std::uint32_t lhs, std::uint32_t rhs, bool operand32) noexcept;
    void logic_flags(std::uint32_t result, bool operand32) noexcept;
    void set_szp(std::uint32_t result, bool operand32) noexcept;
    [[nodiscard]] bool condition(std::uint8_t code) const noexcept;
    void push(std::uint32_t value, bool operand32);
    [[nodiscard]] std::uint32_t pop(bool operand32);

    std::vector<std::uint8_t> memory_;
    Registers regs_{};
    bool halted_{false};
    std::string last_error_{};
};

} // namespace xprevival
