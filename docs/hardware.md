# Booting Sonar on hardware

**Status:** Sonar has only been run in QEMU (the `virt` machine, with
Apple's Hypervisor.framework or TCG). It boots there both directly (`-kernel`)
and through UEFI firmware (edk2). It has **not** been booted on a real
computer. This page describes how a UEFI boot works and what is still
missing for a real laptop.

## The boot disk

`make esp` builds `build/esp.img`: a GPT disk with one EFI System Partition
(FAT32, made by `tools/mkesp.py`) containing:

| Path on the partition | What it is |
| --- | --- |
| `\EFI\BOOT\BOOTAA64.EFI` | the Sonar kernel (`build/Image`), which is also a UEFI application |
| `\sonar\system.img` | the system disk image (SonarFS): settings, files, `/home` |
| `\sonar\model.gguf` | optional, with `make esp MODEL=...`: the DeepSeek model |
| `\sonar\model.gguf.1`, `.2` … | further parts when the model is larger than FAT32's 4 GiB file limit (split into 2 GiB parts) |
| `\EFI\sonar\board.dtb` | optional: a device tree for the machine (see below) |

`\EFI\BOOT\BOOTAA64.EFI` is the file UEFI firmware starts from removable
media on 64-bit Arm when no boot entry is configured. The UEFI specification
defines the path as `\EFI\BOOT\BOOT{machine type short-name}.EFI` (section
"Removable Media Boot Behavior"); GRUB and U-Boot use the AArch64 name
`BOOTAA64.EFI` ([GRUB patch](https://lists.nongnu.org/archive/html/grub-devel/2014-01/msg00013.html),
[U-Boot patch](https://lists.denx.de/pipermail/u-boot/2022-May/483306.html)).

To make a USB stick, write the image to the whole stick (this erases it).
On macOS, find the stick with `diskutil list`, then:

```bash
diskutil unmountDisk /dev/diskN
```

```bash
sudo dd if=build/esp.img of=/dev/rdiskN bs=4m
```

Check `N` carefully: `dd` overwrites whatever disk you name. On Linux use
`lsblk` and `/dev/sdX`. A tool such as balenaEtcher works too.

## What happens at boot

1. The firmware loads `BOOTAA64.EFI`. Sonar's image carries both a Linux
   arm64 `Image` header ([boot protocol](https://docs.kernel.org/arch/arm64/booting.html))
   and a PE/COFF header, so the same file works with `-kernel` and with UEFI.
2. Sonar's UEFI stub (`kernel/arch/arm64/efi.c`) finds:
   - a **device tree**: `\EFI\sonar\board.dtb` if present, else the one the
     firmware provides, else one it **builds from the ACPI tables**
     (`acpi_fdt.c`: CPUs from the MADT, the GIC and its MSI frames, the
     timer from the GTDT, PCIe ECAM windows from the MCFG, a PL011 from the
     SPCR, PSCI from the FADT);
   - the **GOP framebuffer** for the display, when it has a linear one;
   - the time of day from the firmware clock (`GetTime`).
3. It reads `\sonar\system.img` (if there is one) into memory as block
   device `boot0`, and the model (with its split parts) as `boot1`.
4. It writes what it found to `\sonar\boot.log` on the EFI partition
   (`tools/fatcat.py IMAGE /sonar/boot.log` reads it back from an image),
   calls `ExitBootServices`, turns the MMU off and enters the kernel.

### Keeping changes: the system partition

`mkesp.py --system SYSTEM.img` puts the SonarFS volume on the boot disk as
a second partition instead of a file on the EFI partition:

```bash
python3 tools/mkesp.py build/esp.img --system build/system.img EFI/BOOT/BOOTAA64.EFI=build/Image
```

Sonar mounts it from the disk itself (it prefers a SonarFS partition on a
real disk over in-memory copies), so changes are kept across power-off.
That needs a driver for the boot disk: NVMe or virtio work; a USB stick
does not yet (no USB mass storage driver), so when booting from a stick,
keep `sonar/system.img` on the EFI partition, which the loader copies into
RAM (changes are then lost at power-off).

The system partition is also where `update install` puts new kernels
(`/boot/kernel`), which the loader on the EFI partition starts in its
place; `make esp ESP_SYSTEM=1` builds such a disk
([updates.md](updates.md)).

The model is used where the firmware loaded it, so it is not copied again.
A machine needs RAM for the model file plus about 1 GiB (the setup wizard
shows the exact figure).

## Device trees

Sonar learns about hardware from a device tree: memory, CPUs, the
interrupt controller, the timer, PSCI, the serial port, a
`simple-framebuffer`, virtio devices and PCIe host bridges. When the
firmware describes the machine with ACPI only, the UEFI stub builds a
device tree from the ACPI tables, which covers the parts above that ACPI
describes in standard tables (CPUs, GIC, timer, PCIe ECAM, SPCR serial
port). Devices that ACPI describes only in AML (the DSDT), as Windows
laptops do for their I2C keyboards and touchpads, are not seen; such
machines need a DTB at `\EFI\sonar\board.dtb`.

The Linux kernel has device trees for many Arm laptops. For Snapdragon X
(x1e80100) laptops, for example `x1e80100-asus-vivobook-s15.dts`,
`x1e80100-lenovo-yoga-slim7x.dts`, `x1e80100-dell-xps13-9345.dts` and
`x1e78100-lenovo-thinkpad-t14s.dts`
([arch/arm64/boot/dts/qcom](https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/qcom)).
They are compiled with `dtc` from a Linux source tree.

In QEMU, edk2 passed the device tree on only with `-machine virt,acpi=off`;
with ACPI enabled, the edk2 build used here (from QEMU 11.1) gave Sonar no
device tree. The `run-uefi` target sets `acpi=off`.

Apple silicon Macs have no UEFI firmware of their own. Asahi Linux boots
them through m1n1 and U-Boot, which provides UEFI services, passes the
device tree and starts `\EFI\BOOT\BOOTAA64.EFI` from the EFI partition
([boot process guide](https://asahilinux.org/docs/alt/boot-process-guide/),
[quirks](https://asahilinux.org/docs/platform/quirks/)). Sonar could in
principle start the same way. This is untested, and Apple's interrupt
controller (AIC) is not supported (next section).

## Apple's Virtualization framework (Keelson)

Sonar runs as a Linux-type guest under Apple's Virtualization framework,
for example in [Keelson](https://github.com/pronzzz/keelson). It is a good
stand-in for real hardware: the firmware is EDK II with ACPI only, and the
devices sit on PCIe:

| Device | Driver |
| --- | --- |
| PCIe host bridge (ECAM from the ACPI MCFG), MSI-X through the GICv2m frame | `pci.c` |
| virtio-net, virtio-blk, virtio-gpu, entropy over PCI | `virtio.c` (PCI transport) |
| serial console: a virtio console (there is no PL011) | `virtio_console.c` |
| NVMe disk (Keelson `--nvme`) | `nvme.c` |
| USB keyboard and absolute pointer on an xHCI controller | `xhci.c`, `usb_hid.c` |
| time of day: UEFI `GetTime` (there is no PL031) | `timer.c` |

Build a disk image with the system partition and start the VM headless with
its serial console in the terminal:

```bash
python3 tools/mkesp.py Sonar.keelsonvm/disk.img --system build/system.img EFI/BOOT/BOOTAA64.EFI=build/Image
```

```bash
Keelson.app/Contents/MacOS/Keelson --run Sonar.keelsonvm --disk model.gguf:ro
```

`--window` also shows the VM's display in a window (its USB keyboard and
pointer follow the host's while the window has focus), with the serial
console still on stdin and stdout. `--nvme` attaches the boot disk as NVMe, and `--disk PATH[:ro][:usb|:nvme]`
adds more disks. The framework accepts only raw images whose size is a
multiple of 512 bytes, so a GGUF model file attached as a disk may need
padding first (`truncate -s %4096 model.gguf`; the zeros at the end are
ignored). In the window, the virtio-gpu display shows the console or
the desktop, and the USB keyboard and pointer work.

The firmware there keeps 128 KiB at the 4 GiB line, so a 6.5 GiB guest has
its RAM in two pieces (2.3 and 4.3 GiB) and the 4.9 GiB of weights of the
8B model fit in neither. When no single free run is large enough, the AI
engine's large buffers are built from 2 MiB-aligned chunks mapped one after
another in a virtual window above RAM (`kernel/mm/vmap.c`); weights are read
from disk through a 16 MiB contiguous buffer, because disk drivers DMA to
physical addresses.

One quirk found there: the framework handles the virtio `DRIVER_OK` status
write asynchronously and starts each queue from the avail index it finds
then, so a buffer published in that window was silently skipped. The driver
reads the status back after writing it, which waits until it is handled.

## Missing for a real laptop

| Area | Needed for | Status |
| --- | --- | --- |
| USB | xHCI host controller; HID keyboards, mice and touchpads (mouse mode); USB 2 hubs | **works** (tested in the Virtualization framework and QEMU, including hot-plug and devices behind a hub). USB 3 hubs and mass storage are not written |
| Storage | NVMe over PCIe | **works** (tested on the Virtualization framework's NVMe controller: boot disk and data disks, 512- and 4096-byte blocks, clean shutdown at power-off). UFS is not written |
| PCIe | an ECAM window from the device tree or the ACPI MCFG | **works**; controllers that need a vendor driver to bring the link up (e.g. Qualcomm's) are not handled |
| Keyboard and touchpad on the board | Snapdragon X: I2C HID over Qualcomm GENI serial engines; Apple: SPI/MTP | not written. A USB keyboard works |
| Network | Wi-Fi or USB Ethernet | not written; virtio-net only. Wi-Fi is out of reach for now: each chip (Qualcomm ath12k, Intel, MediaTek, Broadcom) needs its vendor's firmware loaded into it and its own driver protocol, plus an 802.11 MAC layer and a WPA2/WPA3 supplicant. USB Ethernet (CDC-ECM/NCM) is the realistic first step |
| Interrupts | GICv3 works; Apple silicon uses AIC | AIC not written, and not testable here: Apple's own hardware needs m1n1/U-Boot, AIC, DART IOMMUs and Apple's NVMe variant, and booting a custom kernel on a Mac requires lowering its boot security. Sonar runs on Apple silicon inside the Virtualization framework instead |
| Serial console | PL011 and virtio console work; laptops use other UARTs (Qualcomm GENI) | not written |
| Firmware entry at EL2 | some firmware enters at EL2 | handled in code, untested |
| HTTPS | TLS for `fetch` and the agent's `web_fetch` | not written |

The device-independent parts do not depend on QEMU: memory management,
multi-core startup through PSCI, SonarFS, the network stack, the DeepSeek
engine, the harness and the desktop (which draws into any linear
framebuffer).
