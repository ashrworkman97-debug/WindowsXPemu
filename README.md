# XP Revival

XP Revival is an original x86 PC emulator project targeting **Windows x64**. Its long-term goal is to boot a real, user-supplied Windows XP installation. It is not an XP desktop mock-up and does not wrap QEMU, Bochs, or another emulator.

> **Development status: early PC emulator.** The project now boots a real two-stage disk image: stage 1 reads sector 1 from the emulated primary-master ATA device using REP INSW, then far-jumps to stage 2. Stage 2 writes to the RAM-backed VGA text display. Windows XP does **not** boot yet.

## Windows target

The project is built and tested with the Windows x64 toolchain in GitHub Actions. The workflow uses Visual Studio's MSVC compiler, runs the CPU and PC boot tests, performs a command-line boot-demo smoke test, and publishes the Windows executables as a build artifact.

Requirements for a local build:
- Windows x64
- Visual Studio 2022 C++ build tools
- CMake 3.20 or newer

## Build on Windows

Open **x64 Native Tools Command Prompt for VS 2022** or a PowerShell terminal where CMake can find Visual Studio:

    cmake -S . -B build -A x64
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure

Run the built-in machine boot demonstration:

    .\build\Release\xprevival.exe --demo

Boot a raw IDE disk image (512-byte sectors, MBR/boot-sector signature 55 AA):

    .\build\Release\xprevival.exe --disk .\disk.img --ram-mib 256

The disk image is read into the emulator's virtual ATA device. Guest writes remain in memory unless you explicitly add --write-disk:

    .\build\Release\xprevival.exe --disk .\disk.img --write-disk

**Warning:** --write-disk writes the guest's modified image back over the named host file. Keep a backup.

The instruction budget can be raised for longer boot sectors with --steps N. Default RAM is 128 MiB and can be selected from 1 to 2048 MiB. The legacy raw-binary CPU harness is still available:

    .\build\Release\xprevival.exe --ram-mib 256 test.bin

## Current implementation

- Configurable RAM backing store (128 MiB default, 1–2048 MiB selectable).
- 32-bit general-purpose registers (EAX–EDI), EIP, EFLAGS, and basic control-register state.
- Real-mode 8-, 16-, and 32-bit instruction paths, initial byte/word/dword MOV operations, ALU operations, branches, stack operations, and 16-bit ModRM effective addresses.
- 32-bit ModRM/SIB effective-address decoding and segment overrides.
- An experimental protected-mode entry subset: LGDT, MOV to/from selected control registers, CR0.PE transition, GDT code/data descriptors, segment-cache reloads, and far jump into a 32-bit code segment.
- Initial 386 4 KiB paging: CR3 page-directory lookup, 10/10/12 address translation, PDE/PTE present and writable checks, CR0.WP supervisor write protection, CR2 page-fault address reporting, page-fault error-code recording, and page-table accessed/dirty bits.
- x86 string operations (MOVS, CMPS, STOS, LODS, SCAS, INS, and OUTS), REP/REPE/REPNE counts, SI/DI direction handling, and DF-controlled traversal. String port input/output uses the CPU's port-I/O bus.
- Deterministic Intel Pentium III CPUID identity, instruction-count RDTSC, basic CLI/STI/CLD/STD controls, and IN/OUT instructions connected to a port-I/O bus.
- Original primary-master ATA PIO device: IDENTIFY DEVICE, 28-bit LBA and basic CHS, READ SECTORS, WRITE SECTORS, FLUSH CACHE, task-file status/error registers, and software reset.
- A connected PC boot path: reads disk sector 0, checks its 55 AA signature, loads it at 0000:7C00, initializes the conventional hard-disk boot registers (including DL=80h), and runs it through the CPU core.
- A text-mode display buffer at physical B8000h. The console renderer reads the guest's character cells from that emulated memory.
- A built-in two-stage raw disk demo. The original 16-bit first-stage boot sector programs the ATA task file, reads LBA 1 to 0000:8000 using REP INSW, and far-jumps to stage 2. The second-stage guest code copies character/attribute cells into VGA text memory and halts. The PC tests verify that sector 1 was fetched through emulated port I/O and that the second stage actually ran.

## Important limitations

This is not yet a complete PC/AT-compatible computer. There is no BIOS firmware or BIOS interrupt service implementation, no interrupt controller or timer, no IRQ delivery, no PCI chipset, no keyboard/mouse, no graphics modes beyond the text-memory buffer, and no ATAPI/CD-ROM device. The direct boot handoff does not emulate the BIOS services expected by most existing MBRs or bootloaders. In particular, this is **not an ISO/Windows XP installer boot path**. Privilege-level enforcement, complete segmentation checks, LDT, TSS/task switching, IDT exception delivery, PSE/PAE, and a TLB are also incomplete.

The current milestone proves that the CPU executes an actual two-stage 16-bit disk boot chain: guest code reads a second sector from the emulated IDE disk via REP INSW, transfers control to it, and writes to the conventional text buffer. It does not prove BIOS compatibility or Windows compatibility.

## Project direction

1. Complete protected-mode privilege checks and exception/interrupt delivery.
2. Implement BIOS firmware services and a realistic PC chipset.
3. Expand IDE/ATAPI and bootable floppy/disk support.
4. Implement VGA hardware behavior plus keyboard/mouse input.
5. Add a Windows configuration UI, virtual disks, ISO selection, and machine controls.
6. Reach Windows XP setup, installation, and desktop compatibility with user-provided media.

## Legal note

The repository does not include Windows, Windows installation media, BIOS ROMs, or proprietary firmware. Users must provide media and firmware they are legally entitled to use. No Microsoft ISO is bundled or redistributed.
