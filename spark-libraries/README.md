
<div align="center">

<img src="asset/spark.png" width="220">

# Spark Programming Language

**Build on it. Command on it.**

</div>

A small collection of libraries and examples for **Spark 0.1**, plus the matching compiler source.

> **Status:** experimental / early beta. These APIs are prototypes, not a stable standard library. Test on disposable files and non-critical systems first.

## Repository layout

```text
spark-libraries/
├── compiler/spc.c       # Spark 0.1 compiler with built-in Linux `sys` module
├── modules/sys_flags.sp # Linux open flags, permission bits, signal constants
├── modules/vector.sp    # SVG vector drawing library
├── examples/            # syscall and vector demonstrations
├── tests/               # small smoke test
├── Makefile
└── README.md
```

## Build

Requirements: Linux and a C compiler (`cc`, GCC, or Clang). The compiler is written in C and translates Spark source into hosted C before invoking the system C compiler.

```sh
make
```

This creates `./spc`. On Android, Termux with Clang can be used. Build and run from Termux's private home directory if shared storage is mounted `noexec`.

## Linux `sys` module

`sys` is built into this particular compiler build; it is not a standalone `.sp` file. Import it in Spark with `$import "sys"`. Import `modules/sys_flags.sp` for common Linux constants:

```spark
$import "sys"
$import "modules/sys_flags.sp"

fn main()
    print("PID: " + str(sys_getpid()) + "\n")
```

| Function | Linux operation / purpose |
|---|---|
| `sys_open(path, flags, mode)` | `openat` — open/create a file |
| `sys_read(fd, max_bytes)` | `read` — read text from a descriptor |
| `sys_read_count()` | Byte count from the previous read |
| `sys_write(fd, text)` | `write` — write a Spark string |
| `sys_close(fd)` | `close` a descriptor |
| `sys_getpid()` / `sys_getppid()` | Process IDs |
| `sys_getuid()` / `sys_getgid()` | User/group IDs |
| `sys_mkdir(path, mode)` | Create a directory |
| `sys_unlink(path)` | Delete a file |
| `sys_kill(pid, signal)` | Send a process signal |
| `sys_sleep_ms(ms)` | Sleep for milliseconds |
| `sys_errno()` | Read the last C `errno` value |

### `sys` limitations and safety

- Linux only. Non-Linux stubs do not provide equivalent Windows functionality.
- This is a hosted userspace syscall wrapper, not raw hardware access or a kernel API.
- `sys_read` is text-only, uses one shared 4096-byte buffer, and clamps reads to 4095 bytes. The next read overwrites the previous result; embedded NUL bytes are not supported as ordinary Spark strings.
- `sys_write` performs one write call. Check its return value because writes can be partial.
- Calls use the permissions of the current process. They do not grant root access or bypass file permissions.
- `sys_kill` can affect other processes. Only signal processes you own or are authorized to manage.
- Check `sys_errno()` immediately after a failing operation.

## Vector library

`modules/vector.sp` writes SVG files through the `sys` module. The following example assumes you run `spc` from the repository root so the module path resolves:

```spark
$import "modules/vector.sp"

fn main()
    vec_begin("drawing.svg", 640, 420)
    vec_background("#101827")
    vec_rect(80, 90, 180, 100, "#38bdf8")
    vec_circle(360, 180, 55, "#facc15")
    vec_text(30, 40, 24, "#ffffff", "Hello from Spark")
    vec_end()
```

Functions: `vec_begin`, `vec_background`, `vec_line`, `vec_rect`, `vec_rect_outline`, `vec_circle`, `vec_circle_outline`, `vec_ellipse`, `vec_text`, `vec_path`, `vec_end`, and `vec_last_error`.

Vector is a **file-based SVG generator**, not a live graphics window or GPU renderer. Coordinates and sizes are integers. `vec_text` currently inserts text without XML escaping; avoid raw `<`, `>`, and `&` in text. `vec_path` accepts raw SVG path data. Use `vec_begin` and `vec_end` once per output document.

## Examples and tests

```sh
make test
```

Or individually:

```sh
./spc examples/sys_demo.sp --run
./spc examples/vector_demo.sp --run
```

The vector example creates `examples/spark-vector-demo.svg`; open it in a browser or SVG viewer.

## Versioning

This repository packages the experimental Spark 0.1 compiler and matching prototype libraries. Keep them together: the built-in `sys` API must match the assumptions in `modules/vector.sp`.
have fun coding sum bs with it❤️❤️❤️❤️
