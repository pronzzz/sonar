# Architecture

Sonar is a single kernel image for 64-bit Arm (AArch64), written in C and
assembly with no third-party code. Everything runs in the kernel at EL1:
there are no user-space processes yet. The shell runs on CPU 0. The other
CPUs wait for parallel work, which is mainly the DeepSeek engine's matrix
multiplications.

```
kernel/
  arch/arm64/   boot.S (entry, EL2→EL1, self-relocation), efi.c (UEFI stub),
                efi_update.c (the stub's update loader), mmu.c, psci.c,
                smp.c, traps.c, vectors.S, linker.ld
  core/         main.c (boot order), console, config, block devices, random
  mm/           pmm.c (page allocator), kmalloc.c
  drivers/      fdt, gic (GICv3), timer + PL031 RTC, pl011, virtio (blk, net,
                gpu, input, rng), simplefb, ramdisk, fbcon (text console)
  fs/           sonarfs.c, file commands
  net/          Ethernet/ARP/IPv4/ICMP/UDP (net.c), tcp.c, dhcp.c, dns.c,
                tls.c (TLS 1.3 client), http.c (HTTP GET with redirects)
  crypto/       SHA-256/512, HMAC, HKDF, ChaCha20-Poly1305, X25519,
                Ed25519, the random number generator
  update/       signed manifests (manifest.c, shared with the UEFI stub),
                the update command (update.c), the public key
  shell/        line editor and shell
  ui/           desktop.c (compositor and windows), gfx.c (drawing)
  ai/           the DeepSeek engine, assistant, harness and tools
tools/          host tools: sonarfs.py, mkesp.py, qemu_session.py, ...
tests/host/     the AI engine, TCP and the crypto built for the host
```

## Boot

1. **Entry.** The image starts with the Linux arm64 `Image` header
   ([booting.rst](https://docs.kernel.org/arch/arm64/booting.html)), so
   loaders that boot Linux (QEMU `-kernel`, U-Boot `booti`) can start it,
   with the device tree address in `x0`. It also has a PE/COFF header, so
   UEFI firmware can run it as `BOOTAA64.EFI` (see
   [hardware.md](hardware.md)). The kernel is linked at address 0 as a
   position-independent executable and applies its own
   `R_AARCH64_RELATIVE` relocations, so it runs wherever it is loaded. The
   build refuses any other kind of dynamic relocation.
   Before anything else, the UEFI stub looks for a signed newer kernel in
   `/boot/kernel` on the SonarFS system partition, verifies it and starts
   it in its place with `LoadImage`/`StartImage`, falling back to itself
   after 3 failed starts ([updates.md](updates.md)).
2. **Exception level.** If entered at EL2, it sets up EL1 and drops to it.
3. **MMU.** It identity-maps RAM from the device tree as normal cached
   memory and the UART as device memory (4 KiB pages, 48-bit addresses),
   then turns on the MMU and caches.
4. **`kmain`** (`kernel/core/main.c`) starts the subsystems in order: UART,
   page allocator, GIC, timer, RTC, interrupts, PSCI and the other CPUs,
   devices from the device tree, file system, settings, network, desktop,
   DeepSeek, and finally the shell.

## Memory

`pmm.c` keeps a sorted list of free physical ranges: the RAM in the device
tree minus the kernel, the device tree, and anything the UEFI stub loaded.
Allocation is first fit, and freed ranges merge with their neighbours, so
multi-GiB model weights and KV caches get exact contiguous runs. When
firmware has split RAM so that no run is large enough, `vmap.c` builds the
buffer from 2 MiB-aligned chunks mapped contiguously in a virtual window at
2^47 (the only place where a virtual address differs from the physical
one; such buffers are not used for DMA). `kmalloc.c` serves requests up to 2,032 bytes from power-of-two size
classes in 64 KiB slabs and larger ones as whole pages. Device registers are mapped on
demand with `ioremap`.

## Interrupts and CPUs

GICv3 (distributor and redistributors from the device tree), the Arm
generic timer, and PSCI (`CPU_ON`) to start the other cores. `parallel_for`
splits a loop across all online CPUs and waits for them. Locks are
spinlocks. There is no scheduler or threads; long operations (a DeepSeek
reply, a download) run on CPU 0 while it polls for Ctrl-C.

## Devices

Devices are found through the device tree (`fdt.c`); on ACPI-only firmware
the UEFI stub builds one from the ACPI tables first (`acpi_fdt.c`).
Drivers:

- **PL011 UART** and **virtio console**: the serial console, with input.
- **virtio** (virtio 1.x, modern only) over MMIO and over PCI: block,
  network, GPU (2D), input (keyboard, mouse, tablet), console and entropy.
- **PCI Express** (`pci.c`): ECAM host bridges, BAR assignment, MSI-X
  through a GICv2m frame or the GICv3 ITS frame described in the tree;
  `lspci` lists the functions.
- **NVMe** (`nvme.c`): admin and I/O queues (polled), every namespace as a
  disk (`nvme0n1`), 512- and 4096-byte blocks, shutdown at power-off.
- **xHCI USB** (`xhci.c`): root ports and USB 2 hubs, hot-plug; **HID**
  (`usb_hid.c`) keyboards (boot protocol or report descriptor), mice,
  touchpads in mouse mode and absolute pointers. Key translation is shared
  with virtio input (`keyboard.c`).
- **simple-framebuffer** or the UEFI GOP framebuffer, for screens without
  virtio-gpu.
- **RAM disks** for images that the UEFI stub loaded into memory.
- **Clock:** a PL031 real-time clock, or the time UEFI reported at boot.

## Console

All output goes through `printk` and the console layer
(`kernel/core/console.c`) to the UART and the framebuffer console
(`fbcon.c`). fbcon draws with Sonar's own 8×16 font and understands the ANSI/VT100
sequences the kernel uses (colors, erasing, cursor movement). It decodes
UTF-8 and shows characters outside the font as a close ASCII equivalent or
a box. The console
keeps a log, which `dmesg` shows and the desktop replays into the Terminal
window. Shell commands can have their output captured, which is how
`bash` tool calls return text to DeepSeek.

## Storage

Block devices (`block.c`) include partitions found from GPT tables.
[SonarFS](sonarfs.md) is the file system, with a metadata journal. At boot
`/` is a SonarFS partition on a real disk if there is one (by its GPT
type), else the first writable SonarFS volume, else an in-memory copy the
UEFI stub loaded; without any, a 64 MiB RAM disk is used and nothing is
saved. Disks are flushed (and NVMe controllers shut down) at power-off. Settings are a key–value file,
`/etc/sonar.conf`, read and changed with `config`.

## Network

The stack in `kernel/net` has Ethernet, ARP, IPv4, ICMP (`ping`), UDP, TCP
(with retransmission, tested against a simulated lossy network by `make
test-net`), DHCP and DNS. `http.c` does HTTP/1.1 GET requests with redirects, over TCP
or over `tls.c`, a TLS 1.3 client with one cipher suite
(ChaCha20-Poly1305, X25519) on the kernel's own crypto (`kernel/crypto`,
tested against OpenSSL's results by `make test-crypto`). TLS does not
verify certificates: it is used for data that is verified some other way,
namely the signed updates (`update`, [updates.md](updates.md)), and by
`fetch`, which warns about it. There is no IPv6.

## DeepSeek

`kernel/ai/` holds an inference engine for GGUF model files:

- `gguf.c` reads the file format; `quant.c` and `qdot.c` implement the
  llama.cpp quantization formats (Q4_0 to Q8_0, the K-quants, F16, BF16),
  with NEON code and the int8 dot-product instructions when the CPU has
  them.
- `model.c` runs llama, qwen2 and qwen3 transformer graphs: RMSNorm,
  grouped-query attention with RoPE, SwiGLU, an fp16 KV cache.
- `tokenizer.c` is a byte-level BPE tokenizer with the models' pre-split
  rules; `chat.c` formats DeepSeek-R1 conversations; `sampler.c` samples
  with temperature and top-p.
- `library.c` finds models (a GGUF file on a whole disk, or files in
  `/models`) and lists the DeepSeek-R1 family with the memory each needs.
- `assistant.c` runs first-time setup (choosing the model, context length
  and reasoning), loads the model, and runs conversations through the
  [harness](harness.md).

The same engine sources build on the host (`make test-host`) to check them
against real models at full speed.

Model weights are read straight from the disk into memory, or used where
UEFI firmware loaded them. The AI code is compiled with `-O3`. The rest of
the kernel uses `-O2 -mstrict-align`, because it may run before the MMU is
on, when unaligned accesses fault.

## Desktop

See [desktop.md](desktop.md).
