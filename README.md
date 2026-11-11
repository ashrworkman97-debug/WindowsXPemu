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

\`\`\`sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/xprevival --ram-mib 256 boot-sector.bin
\`\`\`

On Windows, run \`xprevival.exe\` from the build directory. RAM can be selected from 1 to 2048 MiB; the default is 128 MiB.

## Current prototype

- Configurable RAM backing store (128 MiB default, 1–2048 MiB selectable).
- 32-bit general-purpose register file (EAX–EDI), EIP, and EFLAGS.
- 0x66 operand-size override in the current 16-bit real-mode execution model.
- Initial 8-, 16-, and 32-bit MOV operations, including register and memory operands.
- 16-bit ModRM effective-address forms, signed 8-/16-bit displacements, BP-to-SS default segment behavior, and explicit segment overrides.
- Initial ADD, OR, AND, SUB, XOR, CMP, INC, DEC, PUSH, POP, CALL, RET, and short conditional branches. ADC/SBB and many other x86 operations remain unsupported.
- Moffs accumulator transfers and group-1 immediate arithmetic for the implemented operations.
- Release-safe tests exercise real-mode instruction decoding, byte flags/branches, ModRM RAM access, segment selection, 32-bit data transfers, high RAM, and invalid memory ranges.

**Important limitations:** This is not protected mode. 32-bit effective-address decoding, descriptor tables and hidden segment caches, control registers, A20 gate, paging, interrupts, BIOS, disk controllers, VGA, and ISO boot are not implemented. Windows XP is not yet bootable.

## Legal note

XP Revival does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
