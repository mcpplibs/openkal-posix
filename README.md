# openkal-posix

An implementation of [openkal](https://github.com/mcpplibs/openkal) written above
a POSIX C library.

```toml
[dependencies]
openkal-posix = "0.1.0"
```

## What it is for

openkal's other implementations reach a system through its own interface: Linux's
system calls, Mach and BSD on macOS, the Win32 and object-manager interfaces on
Windows, the supervisor binary interface beneath a bare machine. Some systems
state no such interface. OpenBSD admits system calls only from its C library;
Haiku, GNU/Hurd and illumos are reached through theirs. On those systems the C
library is the stable interface, and an implementation of openkal is written
above it or not at all.

This package is that implementation. It carries a program written to openkal,
and the image runner `okrun`, to any system with a POSIX C library and no
implementation of its own. It is examined on Linux (glibc) and macOS (libSystem),
where the other implementations are the reference against which it is compared.

## Where it may be placed

```toml
provides = ["mcpp:kernel-abi=openkal"]
requires = ["mcpp:c-abi"]
```

It is written above a C library, so it is placed above one: the system's own,
in a program the system's toolchain builds, or beneath `okrun`. It is never
placed beneath a C library that is itself written above openkal, such as
openkal-musl. That C library's functions would call this implementation, and
this implementation would call them back: `kal_time_wall` calls `clock_gettime`,
which would call `kal_time_wall`. The requirement states the arrangement. An
engine that checks that the requirements of the layers form no circle (mcpp
2026.11.10.1, `layer-cycle`) refuses that graph before anything is compiled.

## What it provides

| interface | provided | how |
|---|---|---|
| `openkal.abort` | yes | `_exit`, `abort` |
| `openkal.stream` | yes | descriptors |
| `openkal.memory` | yes | `posix_memalign`; the system's pages from `sysconf` |
| `openkal.env` | yes | the arguments and environment the initialisers are given; `uname` for the system's name |
| `openkal.time` | yes | `clock_gettime`, `nanosleep` |
| `openkal.random` | yes | `getentropy` |
| `openkal.fs` | yes | the `*at` functions relative to directory descriptors; two preopens, the working directory and the root |
| `openkal.process` | yes | `fork` and `fexecve`; process groups as units; SIGINT, SIGTERM and SIGHUP taken over as requests to end |
| `openkal.task` | yes | POSIX threads; a word is waited upon through condition variables |
| `openkal.exec` | yes | `mmap` and `mprotect`, never writable and executable at once |
| `openkal.terminal` | yes | `termios` |
| `openkal.timeout` | yes | `poll`; for streams and programs |
| `openkal.map` | yes | `mmap` of a descriptor |
| `openkal.region` | yes | reserved `PROT_NONE` mappings, backed by `mprotect` |
| `openkal.space` | yes | `fork` |
| `openkal.net` | no | not yet written over sockets |
| `openkal.datagram` | no | as `openkal.net` |
| `openkal.prof` | no | no portable source of samples |
| `openkal.debug` | no | no portable source of symbols |

An interface that is not provided is absent, not stubbed. Its names are not
defined, `kal_interfaces()` does not state it, and the manifest's
`[kernel-abi] provides-interfaces` does not list it (SPEC.md clause 6.1). A
program that calls one fails at link naming it, and a loader that binds by the
list leaves it unbound.

Within a provided interface, a property that the words do not claim is answered
with `kal_err_not_supported`, as clause 6.2 requires. These are:

- confinement, a bound lifetime and a one-string command line for a started
  program;
- the observable units of 0.24 (`kal_process_job_create` and its companions), a
  unit's usage and its limits;
- a rename that does not replace;
- the timeout forms of `accept` and `recv_from`, whose resources belong to the
  absent network interfaces.

**A lock belongs to the open file.** POSIX record locks belong to the process, so
a lock is taken through `F_OFD_SETLK` where the system has locks of the open file
description. Elsewhere a lock on the whole file is BSD's `flock`, which belongs to
the open file on every system that has it.

**A request to end is counted.** `kal_process_stop_requested` installs handlers
for SIGINT, SIGTERM and SIGHUP, unless the starter arranged for one of them to be
ignored. Each request adds one to the word and the word is never zero again, as
openkal 0.25 states. A handler may not call the threads library, so it writes to
a pipe, and a context created when the word is armed wakes its waiters.
`kal_process_terminate` sends SIGTERM, which a program may take over.

## How it is examined

openkal's conformance suite runs for every interface the manifest states, on
Linux with glibc (gcc and llvm) and on macOS with libSystem, through the mtest
engine: one case per interface, with each observation as a part of it. The
suite's three-way check compares the manifest, `kal_interfaces()` and the
exported names. Each job writes mtest's report to its summary, and a final job
joins every job's runs.

On Linux the suite reports 329 observations held, none failed, and 12 not
observed: for interfaces not provided, properties not claimed, and the terminal,
which a run without an interactive stream cannot examine.

## License

Apache-2.0.
