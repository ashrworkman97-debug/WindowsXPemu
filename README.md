# XP Revival

A from-scratch x86 PC emulator project. The goal is to boot a real, user-supplied Windows XP installation inside an emulated PC — not to draw an XP-like desktop and not to wrap QEMU or Bochs.

> **Development status: early CPU prototype.** The core is not yet capable of booting Windows XP, a disk, or an ISO.

## Design goals

- Implement the emulation engine in this repository (no QEMU/Bochs dependency).
- Start from the PC reset state and execute firmware code.
- Build toward a 32-bit x86 CPU with protected mode, paging, interrupts, and the instructions XP needs.
- Provide configurable emulated RAM, then add chipset, timers, PIC/APIC, PCI, IDE/ATAPI, VGA, keyboard, mouse, audio, and networking.
- Boot from user-selected floppy, hard-disk, and CD-ROM images.
- Provide a graphical machine window and configurable virtual hardware.
- Keep disk images and Windows installation media outside the repository.

## Planned milestones

1. **CPU foundation:** 386-width registers, flags, instruction decoding, a real-mode memory bus, ModRM memory operands, and unit tests.
2. **PC startup:** firmware mapping, BIOS services, reset vector, and basic chipset.
3. **Boot devices:** floppy, IDE disk, and ATAPI CD-ROM.
4. **Display and input:** VGA-compatible display, keyboard, and mouse.
5. **Windows-compatible platform:** protected-mode execution, paging, PCI, ACPI/APM as needed, and the remaining device behavior XP requires.
6. **Application:** graphical configuration, ISO selection, virtual disk creation, snapshots, and logs.
7. **Compatibility:** boot and install user-provided Windows XP editions, then test drivers and applications.

## Build

The CPU-core prototype uses C++20 and CMake.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/xprevival --ram-mib 256 boot-sector.bin
```

On Windows, run xprevival.exe from the build directory. RAM can be selected from 1 to 2048 MiB; the default is 128 MiB.

## Current prototype

- Configurable RAM backing store (128 MiB default, 1–2048 MiB selectable).
- 32-bit general-purpose register file (EAX–EDI), EIP, and EFLAGS.
- 0x66 operand-size override while executing with a 16-bit real-mode code-segment default.
- Initial 32-bit immediate MOV, register-direct MOV/ADD/SUB/XOR/CMP, INC/DEC, PUSH/POP, CALL/RET, and short conditional jumps.
- 16-bit real-mode MOV/NOP/HLT subset retained.
- Release-safe tests exercise the 16-bit and 32-bit-width instruction paths, flags, RAM above 1 MiB, and invalid memory ranges.

**Important limitations:** This is not protected mode. Memory-form ModRM operands, 8-bit register operations, 32-bit addressing, A20 behavior, descriptor tables, control registers, paging, interrupts, BIOS, disk controllers, VGA, and ISO boot are not implemented. Widening the register file does not by itself make Windows XP bootable.

## Legal note

XP Revival does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
