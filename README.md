# XP Revival

A from-scratch x86 PC emulator project. The goal is to boot a real, user-supplied Windows XP installation inside an emulated PC — not to draw an XP-like desktop and not to wrap QEMU or Bochs.

> **Development status: early foundation.** This repository is not yet capable of booting Windows XP. The first milestone is a testable CPU/memory core; PC hardware and firmware will be added in stages.

## Design goals

- Implement the emulation engine in this repository (no QEMU/Bochs dependency).
- Start from the PC reset state and execute firmware code.
- Build toward a 32-bit x86 CPU with protected mode, paging, interrupts, and the instructions XP needs.
- Add emulated RAM, chipset, timers, PIC/APIC, PCI, IDE/ATAPI, VGA, keyboard, mouse, audio, and networking incrementally.
- Boot from user-selected floppy, hard-disk, and CD-ROM images.
- Provide a graphical machine window and configurable virtual hardware.
- Keep disk images and Windows installation media outside the repository.

## Planned milestones

1. **CPU foundation:** registers, flags, memory bus, instruction decoding, and unit tests.
2. **PC startup:** reset vector, firmware/BIOS interface, and basic chipset.
3. **Boot devices:** floppy, IDE disk, and ATAPI CD-ROM.
4. **Display and input:** VGA-compatible display, keyboard, and mouse.
5. **Windows-compatible platform:** protected-mode execution, paging, PCI, ACPI/APM as needed, and the remaining device behavior XP requires.
6. **Application:** graphical configuration, ISO selection, virtual disk creation, snapshots, and logs.
7. **Compatibility:** boot and install user-provided Windows XP editions, then test drivers and applications.

## Build

The initial CPU-core milestone uses C++20 and CMake.

```sh
cmake -S . -B build
cmake --build build
./build/xprevival
```

On Windows, run the resulting `xprevival.exe` from the build directory.

## Current prototype

The initial prototype is deliberately small: it provides a 1 MiB real-mode memory space and a minimal 16-bit instruction interpreter so the CPU core can be tested independently before the full PC is implemented. It is not a PC emulator yet and cannot boot an ISO.

## Legal note

XP Revival does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
