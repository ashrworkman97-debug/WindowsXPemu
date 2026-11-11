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
- Initial 386 4 KiB paging: CR3 page-directory lookup, 10/10/12 address translation, PDE/PTE present and writable checks, CR0.WP supervisor write protection, CR2 page-fault address reporting, page-fault error-code recording, and page-table accessed/dirty bits. Tests exercise a mapped non-identity page and a missing-page fault.
- Initial x86 string instruction support: MOVS, CMPS, STOS, LODS, and SCAS byte/word/dword forms; REP/REPE/REPNE counts, SI/DI direction handling, and DF-controlled forward/backward traversal.
- A deterministic virtual Intel Pentium III CPUID identity, a deterministic instruction-count RDTSC counter, basic CLI/STI/CLD/STD flag controls, and byte/word/dword IN/OUT instructions connected to a host-provided port-I/O callback bus.
- An original primary-master ATA PIO device attached to the CPU's port callbacks. It implements IDENTIFY DEVICE, 28-bit LBA and basic CHS addressing, READ SECTORS, WRITE SECTORS, FLUSH CACHE, task-file status/error registers, and software reset. Disk contents are held in a caller-supplied raw image buffer.

## Major missing pieces

This is not a complete 386/Pentium-class PC. Privilege-level enforcement, full segmentation faults and limit enforcement, LDT, TSS/task switching, IDT and interrupt/exception delivery (page faults are recorded and halt the prototype rather than being dispatched through the IDT), PSE/4 MiB pages, PAE, a TLB, A20 behavior, firmware/BIOS services, PCI, PIT/PIC, IRQ routing, secondary ATA channels, slave devices, ATAPI/CD-ROM, DMA, VGA, keyboard, mouse, and a bootable machine configuration are not implemented. The primary ATA device is a directly attached PIO component; the CLI does not yet load a user disk image or boot from it. Windows XP cannot be installed or run yet.

## Project direction

1. Expand the 386 CPU core, segmentation, paging, privilege checks, and exception delivery.
2. Add firmware mapping and BIOS boot services.
3. Implement a PC chipset and bootable floppy/IDE/ATAPI devices.
4. Add VGA-compatible video and keyboard/mouse input.
5. Build the Windows configuration UI, virtual disks, ISO selection, and machine controls.
6. Reach Windows XP setup, installation, and desktop compatibility with user-provided media.

## Legal note

The repository does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
