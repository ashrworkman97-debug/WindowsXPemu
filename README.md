# XP Revival

XP Revival is an original x86 PC emulator project targeting **Windows x64**. Its long-term goal is to boot a real, user-supplied Windows XP installation. It is not an XP desktop mock-up and does not wrap QEMU, Bochs, or another emulator.

> **Development status: early CPU/PC prototype. Windows XP does not boot yet.** There is no BIOS, disk controller, VGA device, ISO boot path, or Windows installer support at this stage.

## Windows target

The project is built and tested with the Windows x64 toolchain in GitHub Actions. The workflow uses Visual Studio's MSVC compiler, runs the CPU test executable, performs a command-line smoke test, and publishes the two Windows executables as a build artifact.

Requirements for a local build:
- Windows x64
- Visual Studio 2022 C++ build tools
- CMake 3.20 or newer

## Build on Windows

Open **x64 Native Tools Command Prompt for VS 2022** or a PowerShell terminal where CMake can find Visual Studio:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\xprevival.exe
```

To run a raw test binary with 256 MiB of emulated RAM:

```powershell
.\build\Release\xprevival.exe --ram-mib 256 test.bin
```

RAM can be selected from 1 to 2048 MiB. The default is 128 MiB. The current command-line harness loads a raw test binary at physical address 0x7C00; it does **not** boot a disk or an ISO.

## Current implementation

- Configurable RAM backing store (128 MiB default, 1–2048 MiB selectable).
- 32-bit general-purpose registers (EAX–EDI), EIP, EFLAGS, and basic control-register state.
- Real-mode 8-, 16-, and 32-bit instruction paths, initial byte/word/dword MOV operations, ALU operations, branches, stack operations, and 16-bit ModRM effective addresses.
- 32-bit ModRM/SIB effective-address decoding and segment overrides.
- An experimental protected-mode entry subset: LGDT, MOV to/from selected control registers, CR0.PE transition, GDT code/data descriptors, segment-cache reloads, and far jump into a 32-bit code segment.

## Major missing pieces

This is not a complete 386 implementation. Privilege checks, full segmentation faults and limit enforcement, LDT, TSS/task switching, IDT and interrupt/exception delivery, paging, A20 behavior, firmware/BIOS services, PCI, PIT/PIC, IDE/ATAPI, VGA, keyboard, mouse, and CD-ROM boot are not implemented. CR0.PG is deliberately rejected. Windows XP cannot be installed or run yet.

## Project direction

1. Expand and test the 386 CPU core and protected-mode behavior.
2. Add firmware mapping and BIOS boot services.
3. Implement a PC chipset and bootable floppy/IDE/ATAPI devices.
4. Add VGA-compatible video and keyboard/mouse input.
5. Build the Windows configuration UI, virtual disks, ISO selection, and machine controls.
6. Reach Windows XP setup, installation, and desktop compatibility with user-provided media.

## Legal note

The repository does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
