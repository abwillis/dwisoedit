# ISO Editor

A graphical editor for ISO 9660 CD/DVD images, built with
[Dynamic Windows](https://dbsoft.org/dwindows.php) for the user interface and
[bkisofs](http://littlesvr.ca/isomaster/) for reading and writing images.

ISO Editor opens an existing image, or starts a new empty one, lets you add,
remove, rename and extract files and folders, and writes the result to a new
image file. It uses a two-pane layout: the local filesystem on the left and
the image contents on the right.

```
+-----------------------------------------------------------+
| Menu: File  Image  Boot  Help                             |
| Toolbar: New Open Save As | Add>> <<Extract | ...         |
+----------------------------+------------------------------+
| Local filesystem           | ISO image                    |
| [path............][Go][Up] | [path.............][Go][Up]  |
| file list                  | file list                    |
+----------------------------+------------------------------+
| status: image name | estimated size | boot record         |
+-----------------------------------------------------------+
```

## Features

- **Open images:** ISO 9660 images, with Rock Ridge and Joliet extensions
  when present. `.nrg` files are offered for opening too, as far as bkisofs
  can read them.
- **Edit contents:**
  - Add files and folders, recursively.
  - Create folders, rename items, delete items.
  - Set POSIX permissions (octal mode).
- **Extract** files and folders from the image to the local filesystem.
- **El Torito boot records:**
  - View boot record information.
  - Extract the boot record to a file.
  - Add a boot record from a file: no emulation, or 1.2 / 1.44 / 2.88 MB
    floppy emulation.
  - Use a file already in the image as a no-emulation boot file (for
    example `isolinux.bin`).
  - Remove the boot record.
- **Image properties:** edit the volume name and publisher; view creation
  time, filename types, boot record and estimated size.
- **Save As** writes ISO 9660 + Rock Ridge + Joliet.
- **Background operations:** Open, Add, Extract and Save run on a worker
  thread, with a progress window and a Cancel button.
- **Safety checks:**
  - Prompts before replacing items already in the image.
  - Skips extraction when the destination already exists.
  - Refuses to save over the image's own source file.
  - Warns before discarding unsaved changes.

## Requirements

- A C compiler with C99 / GNU99 support (GCC or Clang).
- [Dynamic Windows](https://github.com/dbsoft/dwindows), installed so that
  `dwindows-config` is on `PATH`, or with its flags passed manually (see
  [Building](#building)).
- The bkisofs sources. The Makefile builds `bk.a` directly from them.
- POSIX threads.

### Platforms

| Platform | Status |
|---|---|
| Linux, GTK3 backend (openSUSE Tumbleweed) | Built and run during development |
| Other DW backends (GTK4, Windows, macOS, OS/2) | Not tested |

The source uses only portable Dynamic Windows calls, and local paths are
handled with `/` separators, which bkisofs requires. Drive-letter roots such
as `C:/` are recognized for Windows and OS/2 builds.

## Building

```bash
git clone <this-repository>
cd isoedit
make BKDIR=/path/to/bkisofs
```

If Dynamic Windows is not installed with `dwindows-config`, pass its flags
explicitly:

```bash
make BKDIR=/path/to/bkisofs \
     DW_CFLAGS="-I/path/to/dwindows" \
     DW_LIBS="-L/path/to/dwindows -ldwindows"
```

Debug build:

```bash
make clean
make BKDIR=/path/to/bkisofs CC="gcc -g -O0"
```

### Build notes

- **`_FILE_OFFSET_BITS=64` is required.** bkisofs is built with
  `-D_FILE_OFFSET_BITS=64`, and its `VolInfo` structure contains `off_t`
  fields. The application must use the same setting, or the two sides will
  disagree about the structure layout. The Makefile sets it, and
  `isoedit.c` also forces it as a fallback.
- **The bkisofs Makefile is not used.** The Makefile here compiles `bk.a`
  straight from the bkisofs `.c` files, so a bkisofs Makefile whose recipe
  lines lost their tab characters (`missing separator`) does not block the
  build.
- **Tabs:** recipe lines in this project's `Makefile` must start with a real
  tab character.

## Usage

```bash
./isoedit                    # start with a new, empty image
./isoedit /path/to/image.iso # open an image at startup
```

| Action | How |
|---|---|
| Enter a folder | Double-click it, or select it and press Enter |
| Go up a level | Double-click `..`, or click **Up** |
| Jump to a path | Type it in the path field and click **Go** (or press Enter) |
| Add to image | Select local items, then **Add >>** |
| Extract from image | Select image items, then **<< Extract** |
| Context menu | Right-click in either list |
| Open an ISO from the local pane | Double-click a `.iso` or `.nrg` file |

Items are added to the image folder currently shown in the right pane, and
extracted to the local folder currently shown in the left pane. Changes are
held in memory until **Save As** writes a new image file.

### Icons

No icon files are included. At startup the program looks for icons named
`folder` and `file` in the current directory and in the Dynamic Windows
application directory, the same way the Dynamic Windows `dwtest` sample
does. Without them, list rows simply show no icon.

## Design notes

### Threading model

Long operations run on a worker thread created with `dw_thread_new()`.

- **Worker updates the GUI directly.** The worker updates the progress
  window and finishes the job itself (`job_finish()`). On GTK, every Dynamic
  Windows call made from a non-GUI thread takes the GDK lock internally.
- **No timers.** The program uses no Dynamic Windows timer for job handling.
  On GTK3, DW timer callbacks run on the GUI thread without the GDK lock.
  Finishing jobs from a timer left the GDK lock held after a save and froze
  the result dialog.
- **No modal dialogs on the worker.** Nothing that runs on the worker opens
  a modal dialog (`dw_messagebox()`, `dw_file_browse()`, `dw_dialog_wait()`).
  Job results appear in a reusable, non-modal notice window created at
  startup.
- **Main window locked during jobs.** The main window is disabled, and a
  busy flag is set, for the whole job. No other action can touch the image
  while the worker runs.

### Progress window

The progress window is created once and shown or hidden for each job.

- **Never destroyed during a job.** On GTK, the indeterminate progress mode
  (`DW_PERCENT_INDETERMINATE`) starts an internal timeout that keeps a raw
  pointer to the progress bar without holding a reference to it. Destroying
  the window while that timeout was pending caused a crash.
- **No indeterminate mode.** Add and Extract show "item X of N", and Save
  shows the write percentage.

## Known limitations

- **Save As only.** The image can't be saved over the file it was opened
  from, because bkisofs reads unchanged file data from the original image
  while writing.
- **Name conflicts on extract.** Items whose names already exist in the
  local destination are skipped, not overwritten.
- **Image changes are not undoable.**
- **Only partly tested.** Testing so far has been limited to the GTK3
  backend on Linux. Reports from other backends are welcome.

## Troubleshooting

To get a backtrace for a hang or crash on Linux, run the program under gdb.
Starting it under gdb avoids the `ptrace_scope` restriction that blocks
attaching to an already-running process.

```bash
gdb -ex "set pagination off" -ex run --args ./isoedit
# reproduce the problem, press Ctrl+C in the gdb terminal, then:
thread apply all bt
```

When opening an issue, include the full backtrace, the Dynamic Windows
backend in use and its version, and the steps to reproduce.

## License

### ISO Editor

The ISO Editor source code in this repository (`isoedit.c`, `Makefile` and
documentation) is released under the **BSD 3-Clause License**.

    Copyright (c) 2026, abwillis

See [`LICENSE`](LICENSE) for the full text.

### Third-party components

ISO Editor depends on two libraries. Neither is included in this repository,
and each keeps its own license.

| Component | License | Copyright |
|---|---|---|
| bkisofs | GNU General Public License, version 2 only | Andrew Smith |
| Dynamic Windows | 3-clause BSD-style | Brian Smith and contributors (2000-2023) |

#### bkisofs

From the bkisofs `README.md` and the licence header in every source file:

> Any code in this project may be redistributed or modified under the terms
> of the GNU General Public Licence as published by the Free Software
> Foundation; version 2 of the licence.

- **Version 2 only.** The notice names version 2 without "or any later
  version", so it is GPL-2.0-only.
- **Author:** Andrew Smith (http://littlesvr.ca/). Per the source headers,
  the filename mangling code in `bkMangle.c` comes from The Samba Project,
  and Henrique Pinto contributed a bug fix in `bkPath.c`.

#### Dynamic Windows

From `license.txt` in the Dynamic Windows source tree:

    Copyright (c) 2000-2023, Brian Smith <brian@dbsoft.org>
    Copyright (c) 2003-2022, Mark Hessling <mark@rexx.org>
    Copyright (c) 2022, Amr Hesham <amr96@programmer.net>
    Copyright (c) 2019, Anton Popov <panton295@gmail.com>
    Copyright (c) 2017, Ralph Shane <wenjiebiz@outlook.com>
    Copyright (c) 2007, Alex Taylor
    Copyright (c) 2002, Nickolay V. Shmyrev <shmyrev@yandex.ru>
    Copyright (c) 2000, Achim Hasenmueller <achimha@innotek.de>
    Copyright (c) 2000, Peter Nielsen <peter@pmview.com>
    Copyright (c) 1998, Sergey I. Yevtushenko
    All rights reserved.

The license has three conditions:

1. Source redistributions must keep the copyright notice, conditions and
   disclaimer.
2. Binary redistributions must reproduce the copyright notice, conditions
   and disclaimer in the documentation or other accompanying materials.
3. Contributor names may not be used to endorse or promote derived products
   without prior written permission.

See `license.txt` in the Dynamic Windows source tree for the full text.

### Distributing compiled binaries

This repository contains source code only, so the BSD 3-Clause License above
is the only license that applies to its contents. A compiled `isoedit`
binary is potentially different:

- **It is a combined work under GPLv2.** The binary statically links bkisofs
  (`bk.a`). BSD-licensed code may be combined with GPLv2 code, but the
  combined binary as a whole must be distributed under the terms of GPLv2,
  including making the corresponding source code available.
- **Notices must ship with it.** Include all three license texts with any
  binary release:
  - this project's `LICENSE` (BSD 3-Clause),
  - the GPLv2 text (for bkisofs),
  - Dynamic Windows' `license.txt` (its second condition requires this).

This section summarizes the authors' understanding and is not legal advice.

## Credits

- **bkisofs:** Andrew Smith, author of
  [ISO Master](http://littlesvr.ca/isomaster/).
- **Dynamic Windows:** Brian Smith, Mark Hessling and the other contributors
  listed above.
