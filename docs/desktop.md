# The desktop

When Sonar has a display, it starts a windowed desktop. Without one (for
example `make run`, which has only a serial console), it uses the text
console alone.

```bash
make run-desktop
```

```bash
make run-desktop MODEL=path/to/DeepSeek-R1-0528-Qwen3-8B-Q4_K_M.gguf RAM=8G
```

The display can be a virtio-gpu device, or the framebuffer that UEFI
firmware (GOP) or a device tree (`simple-framebuffer`) describes. Input
comes from virtio keyboard, mouse and tablet devices, or from USB
keyboards, mice and absolute pointers (as in Apple's Virtualization
framework). With a tablet or absolute pointer the pointer follows the
host's mouse exactly.

## What is on screen

- **Top bar:** the Sonar mark, the focused window's title, the DeepSeek
  model in use, the network address, memory use and the time (UTC).
- **Dock**, at the bottom: Terminal, Ask DeepSeek, System Monitor, Files,
  About, and Full-screen console. Point at an icon to see its name.
- **Terminal:** the system console, where the shell, `?` questions and
  `dsh` run. It is the same console as on the serial port.
- **System Monitor:** CPUs, memory and disk use, network traffic over the
  last minute, and the DeepSeek model with its last speed in tokens per
  second.
- **Files:** a browser for the file system. Click a folder to open it,
  **Up** to go back, a file to see its size and date. Scroll with the wheel.
- **About:** the version and what the system is.

Windows move by their title bars and close with the round button. Clicking
a window brings it to the front. On wide screens (1100 pixels or more)
System Monitor and Files open beside the Terminal. Screens 2400 pixels or
wider are drawn at double size.

## Keys

| Key | |
| --- | --- |
| F1 | switch between the desktop and the full-screen console |
| F2 | Terminal |
| F3 | System Monitor (show or hide) |
| F4 | Files (show or hide) |
| F5 | About (show or hide) |

Typing always goes to the Terminal. **Ask DeepSeek** types `? ` there, so
you can type a question straight away.

`desktop off` makes the full-screen console the default at boot;
`desktop on` turns the desktop back on (config `ui.desktop`).

## How it works

`kernel/ui/desktop.c` and `kernel/ui/gfx.c`.

- A compositor keeps a back buffer the size of the screen. Changes mark
  damaged rectangles. Only those are redrawn (wallpaper, windows from bottom
  to top, top bar, dock, pointer) and copied to the screen.
- Each window draws into its own surface. The Terminal is the framebuffer
  console (`kernel/drivers/fbcon.c`) pointed at the Terminal window's
  surface instead of the screen, so all console output appears there.
- Interrupt handlers only queue input events. They are handled on the boot
  CPU while the shell waits for input, or while the console flushes text, so
  windows keep updating while DeepSeek is streaming an answer.
- Drawing is software only: rectangles, antialiased rounded corners,
  gradients, shadows and the 8×16 console font. There is no GPU
  acceleration.

The UI has no applications beyond these windows: Sonar has no user-space
programs yet.
