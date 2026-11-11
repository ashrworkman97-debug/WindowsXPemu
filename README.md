# XP Revival

A from-scratch x86 PC emulator project. The goal is to boot a real, user-supplied Windows XP installation inside an emulated PC — not to draw an XP-like desktop and not to wrap QEMU or Bochs.

> **Development status: early CPU prototype.** This repository is not yet capable of booting Windows XP or booting an ISO. The current CPU instruction set is extremely small and real-mode only.

## Design goals

- Implement the emulation engine in this repository (no QEMU/Bochs dependency).
- Start from the PC reset state and execute firmware code.
- Build toward a 32-bit x86 CPU with protected mode, paging, interrupts, and the instructions XP needs.
- Provide configurable emulated RAM, then add chipset, timers, PIC/APIC, PCI, IDE/ATAPI, VGA, keyboard, mouse, audio, and networking.
- Boot from user-selected floppy, hard-disk, and CD-ROM images.
- Provide a graphical machine window and configurable virtual hardware.
- Keep disk images and Windows installation media outside the repository.

## Planned milestones

1. **CPU foundation:** registers, flags, memory bus, instruction decoding, and unit tests.
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
cmake --build build
ctest --test-dir build --output-on-failure
./build/xprevival --ram-mib 256 boot-sector.bin
```

On Windows, run `xprevival.exe` from the build directory. The RAM size can be selected from 1 to 2048 MiB; the default is 128 MiB.

## Current prototype

The emulator now allocates configurable RAM rather than a fixed 1 MiB array. The command-line option `--ram-mib N` selects the backing-store size. Direct memory reads and writes can address RAM above 1 MiB, and tests cover high-memory access and invalid ranges.

**Important limitation:** allocating 128 MiB does not mean the CPU can use all of it while executing guest code yet. The interpreter still supports only a handful of 16-bit real-mode instructions, and protected mode, 32-bit registers/addressing, A20 behavior, BIOS, disk controllers, VGA, and ISO boot are not implemented. This is a memory-foundation step, not an XP-ready emulator.

## Legal note

XP Revival does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
