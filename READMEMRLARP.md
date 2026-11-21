"Spark Logo" (../assets/spark.png)

Sparks Programming Language

A native-compiled programming language for systems programming, networking, graphics, and low-level experimentation.

Sparks is an experimental programming language with a compiler written in C. It translates Sparks source code into C and uses a native C compiler such as GCC or Clang to produce executables.

The project started with Spark 0.1 and has since expanded with memory management, native assembly support, networking, compiler auditing, and experimental firmware tooling.

Latest development version: Sparks 0.11

«Status: Experimental / early beta. Sparks is under active development. Its APIs, compiler behavior, and low-level features may change. Avoid using it for critical systems without extensive testing.»

Features

- Native executable compilation through C
- Indentation-based syntax
- Functions, variables, conditionals, and loops
- Built-in printing and string operations
- Linux system-call wrappers
- TCP networking
- SVG vector graphics library
- Automatic memory cleanup through the Sealer
- SPASM inline assembly and hexadecimal machine-code support
- Detailed compiler audit logs with "--L"
- Secure temporary-file handling and native compiler invocation
- Experimental firmware-image packaging
- ELF packaging through "--tu" (Tux mode)
- Linux and Android Termux development support

Repository layout

spark-libraries/
├── compiler/
│   └── spc.c
├── modules/
│   ├── sys_flags.sp
│   └── vector.sp
├── examples/
├── tests/
├── assets/
│   └── spark.png
├── Makefile
└── README.md

This is the original repository layout. Newer development builds and examples may need to be copied into the repository separately.

Building Sparks

Requirements:

- Linux or Android Termux
- A C compiler such as GCC or Clang
- Standard C development headers and libraries

On Linux:

make

Or compile the current compiler source directly:

cc -O2 compiler/spc.c -o spc

On Termux:

pkg install clang
clang -O2 compiler/spc.c -o spc

The "compiler/spc.c" path assumes that file has been updated to the current compiler version.

Your first Sparks program

Create "hello.sp":

fn main()
    print "Hello from Sparks!"

Compile:

./spc hello.sp

Run:

./hello

Sparks produces a native executable.

The Sealer — automatic memory cleanup

Sparks includes an experimental memory-management mechanism called the Sealer.

The Sealer tracks certain owned values and generates cleanup operations when those values are no longer needed.

For example, compiler-generated C may include:

sp_free(v_response); /* sealed */

The Sealer also manages some temporary values around statements.

This reduces the need for explicit memory cleanup in supported cases.

Important: The Sealer is experimental. Its ownership model has not been formally verified, and it does not guarantee memory safety in every program.

SPASM — Sparks Assembly

SPASM allows low-level machine instructions to be embedded inside Sparks programs.

Example:

fn main()
    spasm
        asm "nop"
        hex "90"

SPASM supports two forms:

- "asm" — native assembly instructions
- "hex" — raw hexadecimal machine-code bytes

On x86-64, the byte "90" represents the NOP instruction.

SPASM is architecture-dependent. Raw machine-code bytes must match the target CPU architecture.

Warning: Invalid assembly or machine-code bytes can crash programs or cause undefined behavior. SPASM is not sandboxed.

Register constraints and more advanced architecture-aware validation remain future development work.

Networking

Sparks supports TCP networking through its "syscalls" module.

Example:

$import "syscalls"

fn main()
    let fd = net_connect("distfiles.gentoo.org", 80)

    if fd < 0
        print sys_error()
        return

    let request = "GET / HTTP/1.0\nHost: distfiles.gentoo.org\nConnection: close\n\n"

    net_send(fd, request)

    let response = net_recv(fd, 4096)

    print response

    net_close(fd)

Compile and run:

./spc net.sp --L
./net

This example has been tested successfully against Gentoo's distfiles server and received an HTTP "200 OK" response.

Networking limitations

- Basic TCP support does not automatically provide HTTPS or TLS.
- A single receive call may return only part of a response.
- Network errors and partial sends must be handled.
- Binary downloads require binary-safe handling.
- HTTP parsing and certificate validation are separate concerns.

Linux "sys" module

The original Spark compiler includes a built-in Linux "sys" module.

Import it using:

$import "sys"

Common Linux constants are provided by:

$import "modules/sys_flags.sp"

Example:

$import "sys"
$import "modules/sys_flags.sp"

fn main()
    print("PID: " + str(sys_getpid()) + "\n")

Available functions

Function| Purpose
"sys_open(path, flags, mode)"| Open or create files
"sys_read(fd, max_bytes)"| Read text from a descriptor
"sys_read_count()"| Get previous read byte count
"sys_write(fd, text)"| Write a string
"sys_close(fd)"| Close a descriptor
"sys_getpid()"| Current process ID
"sys_getppid()"| Parent process ID
"sys_getuid()"| Current user ID
"sys_getgid()"| Current group ID
"sys_mkdir(path, mode)"| Create a directory
"sys_unlink(path)"| Delete a file
"sys_kill(pid, signal)"| Send a signal
"sys_sleep_ms(ms)"| Sleep in milliseconds
"sys_errno()"| Get the last C errno value

Limitations

- The original "sys" module is Linux-specific.
- It is a userspace syscall wrapper, not a bare-metal kernel interface.
- "sys_read" uses a shared text buffer and is not suitable for arbitrary binary data.
- "sys_write" can perform partial writes.
- System calls obey normal operating-system permissions.
- Error handling is the programmer's responsibility.

The older "sys" module and newer "syscalls" networking module should not be assumed to expose identical APIs.

Vector graphics library

The original "modules/vector.sp" library generates SVG files.

Example:

$import "modules/vector.sp"

fn main()
    vec_begin("drawing.svg", 640, 420)
    vec_background("#101827")
    vec_rect(80, 90, 180, 100, "#38bdf8")
    vec_circle(360, 180, 55, "#facc15")
    vec_text(30, 40, 24, "#ffffff", "Hello from Sparks")
    vec_end()

Supported functions include:

"vec_begin", "vec_background", "vec_line", "vec_rect", "vec_rect_outline", "vec_circle", "vec_circle_outline", "vec_ellipse", "vec_text", "vec_path", "vec_end", and "vec_last_error".

This is a file-based SVG generator, not a GPU graphics engine.

Text is not automatically XML-escaped, so special characters require care.

Compiler audit logging — "--L"

Sparks 0.11 supports detailed compiler auditing.

Usage:

./spc hello.sp --L

The compiler saves an audit log:

hello.sp.spc-log.txt

It also automatically displays the log after compilation.

The audit can include:

- Source-file reads
- Lexer tokens
- Parsing operations
- Generated C statements
- Sealer cleanup operations
- Semantic-analysis results
- Temporary-file creation
- Native compiler execution
- Compilation results
- Recorded byte corrections

Example:

[START] Sparks compiler 0.11
[PARSE] Parsed successfully
[SEAL] Free owned variable
[CHECK] Semantic analysis complete: 0 errors, 0 warnings
[NATIVE] Compiler exit status: 0
[BYTE CORRECTIONS] Recorded corrections: 0
[SUMMARY] Compilation: SUCCESS

The "--L" flag records compiler behavior. It does not introduce automatic byte repair.

Security hardening

Recent development versions include security improvements:

- Safer compiler subprocess execution on POSIX systems
- Unpredictable temporary filenames
- File-write error checks
- Native executable hardening options
- Restricted firmware output permissions
- Firmware input validation
- Protection against accidental output overwriting

These protections reduce certain risks but do not make Sparks a sandbox or guarantee that compiled programs are secure.

Programs using raw assembly, syscalls, or external resources still require careful validation.

Firmware image packaging

Sparks includes experimental tooling for packaging existing raw firmware payloads.

Example:

./spc --pack-firmware firmware.raw \
    --target arm64-board \
    -o firmware.bin

The packaging stage can:

- Validate input size
- Reject unsupported executable formats
- Reject symbolic-link inputs
- Prevent accidental overwrites
- Generate an integrity manifest
- Record a CRC32 checksum

Important firmware rules

Sparks never automatically flashes hardware when compiling or packaging an image.

The firmware packager does not yet generate bare-metal machine code directly from ".sp" source.

Target names are currently labels rather than verified board configurations. CRC32 detects accidental corruption but is not a cryptographic signature.

Board-specific memory maps, startup code, linker configurations, and a real bare-metal backend remain future development goals.

Tux mode — "--tu"

Sparks 0.11 introduces experimental ELF packaging for Linux-oriented development.

Normally, the firmware packager rejects ELF executables.

Using "--tu" explicitly allows a structurally valid ELF file to be packaged.

Example:

./spc --pack-firmware kernel.elf \
    --target x86_64-linux \
    -o kernel.img --tu

Tux mode:

- Explicitly enables ELF packaging
- Performs basic ELF validation
- Preserves the original ELF bytes
- Records the ELF format in the manifest
- Does not execute or flash the packaged file

An ELF-containing ".img" file is not automatically a bootable disk image.

Tux mode does not yet create partitions, bootloaders, filesystems, or a complete Linux distribution.

Sparks Package Manager — SPM

SPM is a planned native package manager written using Sparks.

The intended interface is:

./spm install hollywood

The goal is for Sparks itself to manage package downloads, dependency resolution, integrity verification, and installation.

SPM is not yet a complete, tested package installer.

Secure HTTPS connections, signed repository metadata, binary-safe downloads, dependency resolution, and package extraction still need implementation or verification.

Examples and tests

For the original repository:

make test

Individual examples:

./spc examples/sys_demo.sp --run
./spc examples/vector_demo.sp --run

The vector example creates an SVG file that can be opened in a browser.

For current compiler development, additional tests should cover:

- Lexer and parser correctness
- Sealer ownership and cleanup
- SPASM instruction generation
- TCP networking
- Audit logging
- Filename and subprocess security
- Firmware packaging validation
- ELF validation in Tux mode

Version history

Version| Development milestone
0.1| Original compiler, Linux syscalls, vector examples
0.5| Expanded compiler, parsing, code generation, and Sealer functionality
0.6| SPASM assembly and hexadecimal instruction support
0.8| Detailed "--L" audit logging
0.9| POSIX security-hardening improvements
0.10| Experimental secure firmware-image packaging
0.11| Tux mode for ELF packaging

This table summarizes the development versions discussed and tested during the project's evolution. It is not a substitute for a tagged Git release history.

Roadmap

- [ ] Expand automated compiler regression tests
- [ ] Strengthen Sealer ownership guarantees
- [ ] Add architecture-aware SPASM validation
- [ ] Improve binary-safe networking
- [ ] Add authenticated HTTPS support
- [ ] Develop the native SPM package installer
- [ ] Implement a real bare-metal compilation backend
- [ ] Add validated board definitions and memory maps
- [ ] Expand firmware image verification
- [ ] Improve cross-platform support
- [ ] Add more documentation and examples

Contributing

Sparks is experimental, and contributions, bug reports, test programs, and compiler improvements are welcome.

If you find a compiler bug, include the Sparks source that reproduces it, the compiler version, your operating system, and any relevant "--L" audit output.

---

Have fun coding sum bs with it ❤️❤️❤️❤️