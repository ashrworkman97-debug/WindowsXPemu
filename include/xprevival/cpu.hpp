#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace xprevival {

class Cpu {
public:
    using IoReadCallback = std::function<std::uint32_t(std::uint16_t, unsigned)>;
    using IoWriteCallback = std::function<void(std::uint16_t, unsigned, std::uint32_t)>;
    using SoftwareInterruptCallback = std::function<bool(Cpu&, std::uint8_t)>;

    static constexpr std::size_t default_ram_size = 128u * 1024u * 1024u;

    enum Flag : std::uint32_t {
        Carry = 1u << 0, Parity = 1u << 2, AuxiliaryCarry = 1u << 4,
        Zero = 1u << 6, Sign = 1u << 7, Trap = 1u << 8,
        Interrupt = 1u << 9, Direction = 1u << 10, Overflow = 1u << 11
    };

    struct Registers {
        std::uint32_t eax{}, ecx{}, edx{}, ebx{};
        std::uint32_t esp{}, ebp{}, esi{}, edi{};
        std::uint16_t cs{}, ds{}, es{}, ss{}, fs{}, gs{};
        std::uint32_t eip{};
        std::uint32_t eflags{0x00000002};
        std::uint32_t cr0{}, cr2{}, cr3{}, cr4{};
    };

    explicit Cpu(std::size_t ram_bytes = default_ram_size);
    void reset();
    void load(std::span<const std::uint8_t> bytes, std::uint32_t physical_address);
    void set_reset_state();
    void step();
    void run(std::size_t instruction_limit);
    void set_io_callbacks(IoReadCallback read_callback, IoWriteCallback write_callback);
    void set_software_interrupt_callback(SoftwareInterruptCallback callback);

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
    [[nodiscard]] std::uint32_t last_page_fault_error_code() const noexcept { return last_page_fault_error_; }

private:
    enum class SegmentIndex : std::uint8_t { ES, CS, SS, DS, FS, GS };

    struct SegmentCache {
        std::uint32_t base{};
        std::uint32_t limit{0xFFFF};
        bool default32{false};
        bool valid{true};
    };

    struct Operand {
        bool is_register{};
        std::uint8_t reg{};
        std::uint32_t address{};
    };

    struct PageFault {};

    [[nodiscard]] std::uint32_t physical(SegmentIndex segment, std::uint32_t offset) const noexcept;
    [[nodiscard]] std::uint32_t translate_linear(std::uint32_t linear, bool write);
    [[nodiscard]] std::uint32_t read_port(std::uint16_t port, unsigned width);
    void write_port(std::uint16_t port, unsigned width, std::uint32_t value);
    [[noreturn]] void record_page_fault(std::uint32_t linear, bool write, bool present, bool reserved = false);
    [[nodiscard]] std::uint8_t read_linear8(std::uint32_t address);
    [[nodiscard]] std::uint16_t read_linear16(std::uint32_t address);
    [[nodiscard]] std::uint32_t read_linear32(std::uint32_t address);
    void write_linear8(std::uint32_t address, std::uint8_t value);
    void write_linear16(std::uint32_t address, std::uint16_t value);
    void write_linear32(std::uint32_t address, std::uint32_t value);
    [[nodiscard]] bool load_segment(SegmentIndex segment, std::uint16_t selector);
    [[nodiscard]] std::uint8_t fetch8();
    [[nodiscard]] std::uint16_t fetch16();
    [[nodiscard]] std::uint32_t fetch32();
    [[nodiscard]] std::uint32_t read_general(std::uint8_t index, bool operand32) const noexcept;
    void write_general(std::uint8_t index, bool operand32, std::uint32_t value) noexcept;
    [[nodiscard]] std::uint8_t read_byte_register(std::uint8_t index) const noexcept;
    void write_byte_register(std::uint8_t index, std::uint8_t value) noexcept;
    [[nodiscard]] bool decode_modrm(Operand& rm, std::uint8_t& reg, bool address32,
                                    bool has_segment_override, SegmentIndex segment_override);
    [[nodiscard]] std::uint32_t read_operand(const Operand& operand, unsigned width);
    void write_operand(const Operand& operand, unsigned width, std::uint32_t value);
    [[nodiscard]] std::uint32_t add(std::uint32_t lhs, std::uint32_t rhs, unsigned width) noexcept;
    [[nodiscard]] std::uint32_t subtract(std::uint32_t lhs, std::uint32_t rhs, unsigned width) noexcept;
    void logic_flags(std::uint32_t result, unsigned width) noexcept;
    void set_szp(std::uint32_t result, unsigned width) noexcept;
    [[nodiscard]] bool condition(std::uint8_t code) const noexcept;
    void push(std::uint32_t value, bool operand32);
    [[nodiscard]] std::uint32_t pop(bool operand32);

    std::vector<std::uint8_t> memory_;
    SegmentCache segments_[6]{};
    std::uint32_t gdtr_base_{};
    std::uint16_t gdtr_limit_{};
    std::uint32_t last_page_fault_error_{};
    std::uint64_t tsc_{};
    IoReadCallback io_read_callback_{};
    IoWriteCallback io_write_callback_{};
    SoftwareInterruptCallback software_interrupt_callback_{};
    Registers regs_{};
    bool halted_{false};
    std::string last_error_{};
};

} // namespace xprevival
