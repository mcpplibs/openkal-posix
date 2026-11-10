// openkal-posix: openkal's interfaces written above a POSIX C library.
//
// The specification lets an implementation be built upon a C library, beneath
// one, or without one; this one is built upon the system's own. It carries a
// program written to openkal --- and the image runner, okrun --- to a system
// that has no implementation of its own, at the price every implementation
// above a C library pays: it cannot be placed beneath a C library written above
// openkal (mcpp.toml says why).
//
// An interface this file does not implement is absent, not stubbed: its names
// are not defined, and kal_interfaces() and the manifest do not state it
// (SPEC.md clause 6.1). Within a stated interface, an operation the property
// words do not claim answers kal_err_not_supported, as clause 6.2 requires.
#include <openkal.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <pthread_np.h>
#endif
#if defined(__APPLE__)
#include <sys/random.h>   // getentropy
#endif

extern char** environ;

namespace {

// ---------------------------------------------------------------- handles
// A handle is a descriptor plus one, so that zero is never a valid handle.
inline kal_uintptr pack(int fd) { return fd < 0 ? 0 : static_cast<kal_uintptr>(fd) + 1u; }
inline int unpack(kal_uintptr h) { return h == 0 ? -1 : static_cast<int>(h - 1u); }

int translate(int e) {
    switch (e) {
        case 0:            return kal_ok;
        case EAGAIN:       return kal_err_again;
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:  return kal_err_again;
#endif
        case ENOMEM:       return kal_err_no_memory;
        case ENOSPC:       return kal_err_no_space;
        case EDQUOT:       return kal_err_no_space;
        case EACCES:
        case EPERM:
        case EROFS:        return kal_err_permission;
        case ENOSYS:
        case ENOTSUP:      return kal_err_not_supported;
        case EPIPE:
        case ECONNRESET:   return kal_err_closed;
        case ENOENT:       return kal_err_not_found;
        case EEXIST:       return kal_err_exists;
        case ENOTEMPTY:    return kal_err_not_empty;
        case EISDIR:       return kal_err_is_directory;
        case ENOTDIR:      return kal_err_not_directory;
        case ENOEXEC:      return kal_err_not_program;
        case EBADF:
        case EINVAL:
        case ENAMETOOLONG:
        case ELOOP:        return kal_err_invalid;
        default:           return kal_err_io;
    }
}
inline int fail() { return translate(errno); }
inline kal_intptr nfail() { return -static_cast<kal_intptr>(translate(errno)); }

// A name of openkal's (bytes and a length) as a terminated string.
struct cname {
    char buf[PATH_MAX];
    bool ok = false;
    cname(const char* s, kal_uintptr n) {
        if (s == nullptr || n == 0 || n >= sizeof buf) return;
        if (s[0] == '/') return;                       // shall not begin with a separator
        memcpy(buf, s, n); buf[n] = '\0';
        if (memchr(buf, '\0', n)) return;
        // shall not contain a component that ascends
        for (const char* p = buf; *p;) {
            const char* e = strchr(p, '/'); size_t l = e ? size_t(e - p) : strlen(p);
            if (l == 2 && p[0] == '.' && p[1] == '.') return;
            p += l; if (*p == '/') ++p;
        }
        ok = true;
    }
};

kal_intptr put(const char* v, kal_uintptr n, char* out, kal_uintptr cap) {
    if (out && cap) memcpy(out, v, n < cap ? n : cap);
    return static_cast<kal_intptr>(n);
}

// ---------------------------------------------------------------- arguments
int    g_argc = 0;
char** g_argv = nullptr;
char** g_envp = nullptr;

// glibc, FreeBSD, NetBSD and OpenBSD pass (argc, argv, envp) to the
// initialisers of the executable; the arguments are recorded here.
__attribute__((constructor)) void record(int argc, char** argv, char** envp) {
    g_argc = argc; g_argv = argv; g_envp = envp ? envp : environ;
}

}  // namespace

extern "C" {

// ================================================================ version
kal_u64 kal_version(void) { return KAL_VERSION; }
kal_u64 kal_interfaces(void) {
    return KAL_IFACE_ABORT | KAL_IFACE_STREAM | KAL_IFACE_MEMORY | KAL_IFACE_ENV
         | KAL_IFACE_TIME | KAL_IFACE_RANDOM | KAL_IFACE_FS | KAL_IFACE_PROCESS
         | KAL_IFACE_TASK | KAL_IFACE_EXEC | KAL_IFACE_TERMINAL | KAL_IFACE_TIMEOUT
         | KAL_IFACE_MAP | KAL_IFACE_REGION
         | KAL_IFACE_SPACE
         ;
}

// ================================================================ abort
[[noreturn]] void kal_abort(const char* msg, kal_uintptr len) {
    if (msg && len) { ssize_t r = write(2, msg, len); (void)r; ssize_t s = write(2, "\n", 1); (void)s; }
    abort();
}
[[noreturn]] void kal_exit(int code) { _exit(code); }

// ================================================================ stream
kal_stream kal_stdin (void) { return kal_stream{ pack(0) }; }
kal_stream kal_stdout(void) { return kal_stream{ pack(1) }; }
kal_stream kal_stderr(void) { return kal_stream{ pack(2) }; }

kal_intptr kal_stream_write(kal_stream s, const void* buf, kal_uintptr len) {
    const int fd = unpack(s.h);
    if (fd < 0) return -kal_err_invalid;
    const char* p = static_cast<const char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const ssize_t r = write(fd, p + done, len - done);
        if (r < 0) { if (errno == EINTR) continue; return done ? kal_intptr(done) : nfail(); }
        done += kal_uintptr(r);
    }
    return kal_intptr(done);
}
kal_intptr kal_stream_read(kal_stream s, void* buf, kal_uintptr len) {
    const int fd = unpack(s.h);
    if (fd < 0) return -kal_err_invalid;
    for (;;) {
        const ssize_t r = read(fd, buf, len);
        if (r >= 0) return kal_intptr(r);
        if (errno != EINTR) return nfail();
    }
}
int kal_stream_flush(kal_stream s) { return unpack(s.h) < 0 ? kal_err_invalid : kal_ok; }
kal_uintptr kal_stream_props(kal_stream s) {
    const int fd = unpack(s.h);
    return (fd >= 0 && isatty(fd)) ? KAL_STREAM_PROP_INTERACTIVE : 0;
}

// ================================================================ memory
void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = nullptr;
    if (posix_memalign(&p, align, size ? size : 1) != 0) return nullptr;
    return p;
}
void kal_free(void* p, kal_uintptr, kal_uintptr) { free(p); }
kal_uintptr kal_memory_granularity(void) { return kal_uintptr(sysconf(_SC_PAGESIZE)); }
int kal_memory_info(kal_memory_status* out) {
    if (!out || out->self_size < 8) return kal_err_invalid;
    // What the system states: its pages, and those it holds free where it says.
    const long page = sysconf(_SC_PAGESIZE);
    kal_memory_status m{};
    m.self_size = out->self_size;
#if defined(_SC_PHYS_PAGES)
    const long total = sysconf(_SC_PHYS_PAGES);
    if (total > 0 && page > 0) { m.total_bytes = kal_u64(total) * kal_u64(page); m.present |= KAL_MSTATUS_TOTAL; }
#endif
#if defined(_SC_AVPHYS_PAGES)
    const long avail = sysconf(_SC_AVPHYS_PAGES);
    if (avail > 0 && page > 0) { m.available_bytes = kal_u64(avail) * kal_u64(page); m.present |= KAL_MSTATUS_AVAILABLE; }
#endif
    if ((m.present & KAL_MSTATUS_TOTAL) && (m.present & KAL_MSTATUS_AVAILABLE) && m.available_bytes > m.total_bytes)
        m.available_bytes = m.total_bytes;
    // No more of the structure than the caller stated.
    if (m.self_size < offsetof(kal_memory_status, total_bytes) + 8) m.present &= ~KAL_MSTATUS_TOTAL;
    if (m.self_size < offsetof(kal_memory_status, available_bytes) + 8) m.present &= ~KAL_MSTATUS_AVAILABLE;
    const kal_u32 cap = m.self_size;
    memcpy(out, &m, cap < sizeof m ? cap : sizeof m);
    return kal_ok;
}

// ================================================================ env
kal_uintptr kal_env_arg_count(void) { return kal_uintptr(g_argc); }
kal_intptr kal_env_arg(kal_uintptr i, char* out, kal_uintptr cap) {
    if (i >= kal_uintptr(g_argc)) return -kal_err_invalid;
    const char* a = g_argv[i] ? g_argv[i] : "";
    return put(a, strlen(a), out, cap);
}
static char** env_vec() { return g_envp ? g_envp : environ; }
kal_intptr kal_env_var(const char* name, kal_uintptr n, char* out, kal_uintptr cap) {
    for (char** e = env_vec(); e && *e; ++e)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return put(*e + n + 1, strlen(*e + n + 1), out, cap);
    return -kal_err_not_found;
}
kal_uintptr kal_env_var_count(void) { kal_uintptr n = 0; for (char** e = env_vec(); e && *e; ++e) ++n; return n; }
kal_intptr kal_env_var_at(kal_uintptr i, char* out, kal_uintptr cap) {
    char** e = env_vec();
    for (kal_uintptr k = 0; e && e[k]; ++k)
        if (k == i) { const char* q = strchr(e[k], '='); return put(e[k], q ? kal_uintptr(q - e[k]) : strlen(e[k]), out, cap); }
    return -kal_err_invalid;
}
kal_intptr kal_env_image(char* out, kal_uintptr cap) {
    char buf[PATH_MAX];
#if defined(__linux__)
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return put(buf, kal_uintptr(n), out, cap);
#endif
    if (g_argc > 0 && g_argv[0] && realpath(g_argv[0], buf)) return put(buf, strlen(buf), out, cap);
    return -kal_err_not_found;
}
kal_intptr kal_env_loader(char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_system(char* out, kal_uintptr cap) {
    struct utsname u;
    if (uname(&u) != 0) return -kal_err_not_found;
    char s[sizeof u.sysname];
    size_t n = 0;
    for (; u.sysname[n] && n < sizeof s; ++n) { char c = u.sysname[n]; s[n] = (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
    if (n == 6 && memcmp(s, "darwin", 6) == 0) return put("macos", 5, out, cap);
    return put(s, n, out, cap);
}
// THE RESOURCE PREFERENCE (0.23), which a start conveys in KAL_RESOURCES, as
// the other implementations convey it: `performance', `economy', or balanced.
int kal_env_resources(void) {
    static int known = -1;
    int v = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (v >= 0) return v;
    v = KAL_RESOURCES_BALANCED;
    char buf[32];
    const kal_intptr n = kal_env_var("KAL_RESOURCES", 13, buf, sizeof buf - 1);
    if (n > 0 && n < kal_intptr(sizeof buf)) {
        buf[n] = 0;
        if (strcmp(buf, "performance") == 0) v = KAL_RESOURCES_PERFORMANCE;
        else if (strcmp(buf, "economy") == 0) v = KAL_RESOURCES_ECONOMY;
    }
    __atomic_store_n(&known, v, __ATOMIC_RELAXED);
    return v;
}

// ================================================================ time
static kal_u64 ns_of(clockid_t c) { timespec t; clock_gettime(c, &t); return kal_u64(t.tv_sec) * 1000000000ull + kal_u64(t.tv_nsec); }
kal_duration kal_time_monotonic(void) { return ns_of(CLOCK_MONOTONIC); }
kal_duration kal_time_wall(void) { return ns_of(CLOCK_REALTIME); }
kal_duration kal_time_monotonic_granularity(void) {
    timespec t; if (clock_getres(CLOCK_MONOTONIC, &t) != 0) return 1;
    const kal_u64 g = kal_u64(t.tv_sec) * 1000000000ull + kal_u64(t.tv_nsec);
    return g ? g : 1;
}
void kal_time_sleep(kal_duration ns) {
    timespec t{ time_t(ns / 1000000000ull), long(ns % 1000000000ull) };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}
kal_uintptr kal_time_props(void) { return KAL_TIME_PROP_WALL_AVAILABLE | KAL_TIME_PROP_SLEEP_PRECISE; }

// ================================================================ random
int kal_random_fill(void* out, kal_uintptr len) {
    unsigned char* p = static_cast<unsigned char*>(out);
    while (len) {
        const size_t n = len > 256 ? 256 : len;
        if (getentropy(p, n) != 0) return kal_err_io;
        p += n; len -= n;
    }
    return kal_ok;
}
kal_uintptr kal_random_props(void) { return 0; }

// ================================================================ fs
namespace {
struct preopen { char name[PATH_MAX]; size_t len; int fd; };
preopen g_pre[2];
kal_uintptr g_pre_n = 0;
pthread_once_t g_pre_once = PTHREAD_ONCE_INIT;
const int dir_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
void open_preopens() {
    // Two, as openkal-linux has them: the working directory, by its name, and
    // the root.
    if (!getcwd(g_pre[0].name, sizeof g_pre[0].name)) strcpy(g_pre[0].name, ".");
    g_pre[0].len = strlen(g_pre[0].name);
    g_pre[0].fd  = open(".", dir_flags);
    strcpy(g_pre[1].name, "/"); g_pre[1].len = 1;
    g_pre[1].fd  = open("/", dir_flags);
    g_pre_n = 2;
}
int kind_of(mode_t m) {
    if (S_ISREG(m)) return kal_node_file;
    if (S_ISDIR(m)) return kal_node_directory;
    if (S_ISLNK(m)) return kal_node_link;
    return kal_node_other;
}
// The fields an enquiry fills, no more of the structure than the caller stated
// (`self_size'), and, where `mask' is not all of them, only the fields asked for.
void fill_info(const struct stat& st, kal_node_info* out, kal_u32 mask = ~kal_u32{0}) {
    const kal_u32 cap = out->self_size;
    kal_node_info i{};
    i.self_size = cap;
    i.present = KAL_INFO_KIND | KAL_INFO_SIZE | KAL_INFO_MODIFIED | KAL_INFO_WRITABLE | KAL_INFO_IDENTITY;
    i.kind = kind_of(st.st_mode);
    i.size = kal_u64(st.st_size);
#if defined(__APPLE__)
    i.modified_ns = kal_u64(st.st_mtimespec.tv_sec) * 1000000000ull + kal_u64(st.st_mtimespec.tv_nsec);
#else
    i.modified_ns = kal_u64(st.st_mtim.tv_sec) * 1000000000ull + kal_u64(st.st_mtim.tv_nsec);
#endif
    i.identity[0] = kal_u64(st.st_dev);
    i.identity[1] = kal_u64(st.st_ino);
    i.writable = (st.st_mode & 0222) != 0;
    if (i.kind == kal_node_file) { i.present |= KAL_INFO_EXECUTABLE; i.executable = (st.st_mode & 0111) != 0; }
    i.present &= mask | KAL_INFO_KIND;
    struct { kal_u32 bit; size_t end; } const fields[] = {
        { KAL_INFO_SIZE,       offsetof(kal_node_info, size) + 8 },
        { KAL_INFO_MODIFIED,   offsetof(kal_node_info, modified_ns) + 8 },
        { KAL_INFO_IDENTITY,   offsetof(kal_node_info, identity) + 16 },
        { KAL_INFO_KIND,       offsetof(kal_node_info, kind) + sizeof(int) },
        { KAL_INFO_WRITABLE,   offsetof(kal_node_info, writable) + sizeof(int) },
        { KAL_INFO_EXECUTABLE, offsetof(kal_node_info, executable) + sizeof(int) },
    };
    for (const auto& f : fields) if (cap < f.end) i.present &= ~f.bit;
    if (!(i.present & KAL_INFO_SIZE)) i.size = 0;
    if (!(i.present & KAL_INFO_MODIFIED)) i.modified_ns = 0;
    if (!(i.present & KAL_INFO_IDENTITY)) i.identity[0] = i.identity[1] = 0;
    if (!(i.present & KAL_INFO_WRITABLE)) i.writable = 0;
    if (!(i.present & KAL_INFO_EXECUTABLE)) i.executable = 0;
    memcpy(out, &i, cap < sizeof i ? cap : sizeof i);
}
struct lister { DIR* d; };
}  // namespace

kal_uintptr kal_fs_preopen_count(void) { pthread_once(&g_pre_once, open_preopens); return g_pre_n; }
int kal_fs_preopen(kal_uintptr i, kal_dir* out, char* name, kal_uintptr cap, kal_uintptr* len) {
    pthread_once(&g_pre_once, open_preopens);
    if (i >= g_pre_n || !out) return kal_err_invalid;
    if (g_pre[i].fd < 0) return kal_err_permission;
    *out = kal_dir{ pack(g_pre[i].fd) };
    put(g_pre[i].name, g_pre[i].len, name, cap);
    if (len) *len = g_pre[i].len;
    return kal_ok;
}
kal_uintptr kal_fs_props(kal_dir) {
    return KAL_FS_PROP_CASE_SENSITIVE | KAL_FS_PROP_LINKS | KAL_FS_PROP_MODIFIED_TIME
         | KAL_FS_PROP_ATOMIC_RENAME | KAL_FS_PROP_MAKE_LINKS | KAL_FS_PROP_LOCKS
         | KAL_FS_PROP_CAPACITY | KAL_FS_PROP_EXECUTABLE | KAL_FS_PROP_REMOVE_OPEN
         | KAL_FS_PROP_HARD_LINKS;
}
kal_uintptr kal_fs_max_name(void) { return PATH_MAX - 1; }

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len, kal_dir* out) {
    const int b = unpack(base.h); cname n(name, len);
    if (b < 0 || !out || !n.ok) return kal_err_invalid;
    const int fd = openat(b, n.buf, dir_flags);
    if (fd < 0) return fail();
    *out = kal_dir{ pack(fd) };
    return kal_ok;
}
int kal_fs_open(kal_dir base, const char* name, kal_uintptr len, kal_uintptr f, kal_file* out) {
    const int b = unpack(base.h); cname n(name, len);
    if (b < 0 || !out || !n.ok) return kal_err_invalid;
    int o = O_CLOEXEC;
    const bool r = f & KAL_OPEN_READ, w = f & (KAL_OPEN_WRITE | KAL_OPEN_APPEND);
    o |= (r && w) ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    if (f & KAL_OPEN_CREATE)    o |= O_CREAT;
    if (f & KAL_OPEN_EXCLUSIVE) o |= O_EXCL;
    if (f & KAL_OPEN_TRUNCATE)  o |= O_TRUNC;
    if (f & KAL_OPEN_APPEND)    o |= O_APPEND;
    const int fd = openat(b, n.buf, o, 0666);
    if (fd < 0) return fail();
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); return kal_err_is_directory; }
    *out = kal_file{ pack(fd) };
    return kal_ok;
}
void kal_fs_close_dir (kal_dir d)  { const int fd = unpack(d.h); if (fd >= 0) close(fd); }
void kal_fs_close_file(kal_file f) { const int fd = unpack(f.h); if (fd >= 0) close(fd); }
kal_stream kal_fs_stream(kal_file f) { return kal_stream{ f.h }; }
int kal_fs_seek(kal_file f, kal_i64 off, int whence, kal_u64* result) {
    const int w = whence == KAL_SEEK_SET ? SEEK_SET : whence == KAL_SEEK_CURRENT ? SEEK_CUR : SEEK_END;
    const off_t r = lseek(unpack(f.h), off_t(off), w);
    if (r < 0) return fail();
    if (result) *result = kal_u64(r);
    return kal_ok;
}
int kal_fs_truncate(kal_file f, kal_u64 size) { return ftruncate(unpack(f.h), off_t(size)) == 0 ? kal_ok : fail(); }
kal_intptr kal_fs_read_at(kal_file f, void* buf, kal_uintptr len, kal_u64 off) {
    for (;;) { const ssize_t r = pread(unpack(f.h), buf, len, off_t(off)); if (r >= 0) return r; if (errno != EINTR) return nfail(); }
}
kal_intptr kal_fs_write_at(kal_file f, const void* buf, kal_uintptr len, kal_u64 off) {
    kal_uintptr done = 0; const char* p = static_cast<const char*>(buf);
    while (done < len) {
        const ssize_t r = pwrite(unpack(f.h), p + done, len - done, off_t(off + done));
        if (r < 0) { if (errno == EINTR) continue; return done ? kal_intptr(done) : nfail(); }
        done += kal_uintptr(r);
    }
    return kal_intptr(done);
}
int kal_fs_info(kal_dir base, const char* name, kal_uintptr len, kal_uintptr flags, kal_u32, kal_node_info* out) {
    const int b = unpack(base.h); cname n(name, len);
    if (b < 0 || !out || !n.ok) return kal_err_invalid;
    struct stat st;
    if (fstatat(b, n.buf, &st, (flags & KAL_FS_NO_RESOLVE) ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
        if (errno == ENOENT) {
            const kal_u32 cap = out->self_size; kal_node_info i{}; i.self_size = cap;
            i.present = KAL_INFO_KIND; i.kind = kal_node_absent;
            memcpy(out, &i, cap < sizeof i ? cap : sizeof i);
            return kal_ok;
        }
        return fail();
    }
    fill_info(st, out);
    return kal_ok;
}
int kal_fs_file_info(kal_file f, kal_u32, kal_node_info* out) {
    struct stat st;
    if (!out || fstat(unpack(f.h), &st) != 0) return out ? fail() : kal_err_invalid;
    fill_info(st, out);
    return kal_ok;
}
int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    cname n(name, len); if (!n.ok) return kal_err_invalid;
    return mkdirat(unpack(base.h), n.buf, 0777) == 0 ? kal_ok : fail();
}
int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    cname n(name, len); if (!n.ok) return kal_err_invalid;
    const int b = unpack(base.h);
    if (unlinkat(b, n.buf, 0) == 0) return kal_ok;
    if (errno == EISDIR || errno == EPERM) { if (unlinkat(b, n.buf, AT_REMOVEDIR) == 0) return kal_ok; }
    return fail();
}
int kal_fs_rename(kal_dir from, const char* a, kal_uintptr al, kal_dir to, const char* b, kal_uintptr bl) {
    cname x(a, al), y(b, bl); if (!x.ok || !y.ok) return kal_err_invalid;
    return renameat(unpack(from.h), x.buf, unpack(to.h), y.buf) == 0 ? kal_ok : fail();
}
int kal_fs_rename_flags(kal_dir from, const char* a, kal_uintptr al, kal_dir to, const char* b, kal_uintptr bl, kal_uintptr flags) {
    if (flags == 0) return kal_fs_rename(from, a, al, to, b, bl);
    return flags == KAL_RENAME_NO_REPLACE ? kal_err_not_supported : kal_err_invalid;
}
static int set_time(int dirfd, const char* name, int fd, kal_u64 ns) {
    timespec t[2]; t[0].tv_sec = 0; t[0].tv_nsec = UTIME_OMIT;
    t[1].tv_sec = time_t(ns / 1000000000ull); t[1].tv_nsec = long(ns % 1000000000ull);
    const int r = name ? utimensat(dirfd, name, t, 0) : futimens(fd, t);
    return r == 0 ? kal_ok : fail();
}
int kal_fs_set_modified(kal_file f, kal_u64 ns) { return set_time(-1, nullptr, unpack(f.h), ns); }
int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len, kal_u64 ns) {
    cname n(name, len); if (!n.ok) return kal_err_invalid;
    return set_time(unpack(base.h), n.buf, -1, ns);
}
int kal_fs_set_executable_at(kal_dir base, const char* name, kal_uintptr len, int executable) {
    cname n(name, len); if (!n.ok) return kal_err_invalid;
    struct stat st; const int b = unpack(base.h);
    if (fstatat(b, n.buf, &st, 0) != 0) return fail();
    if (S_ISDIR(st.st_mode)) return kal_err_is_directory;
    mode_t m = st.st_mode & 07777;
    if (executable) m |= (m & 0444) >> 2; else m &= ~mode_t(0111);
    return fchmodat(b, n.buf, m, 0) == 0 ? kal_ok : fail();
}
// A LOCK BELONGS TO THE OPEN FILE, as openkal states it: a second open of the
// same name in the same program is refused it. POSIX's record locks belong to
// the process, so they are taken where the system has locks of the open file
// description (F_OFD_SETLK), and a lock upon the whole file is BSD's flock,
// which belongs to the open file everywhere.
int kal_fs_lock(kal_file f, kal_u64 start, kal_u64 len, kal_uintptr mode) {
    const int fd = unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    const kal_uintptr kinds = mode & (KAL_LOCK_SHARED | KAL_LOCK_EXCLUSIVE);
    if (kinds != KAL_LOCK_SHARED && kinds != KAL_LOCK_EXCLUSIVE) return kal_err_invalid;
    if (mode & ~(KAL_LOCK_SHARED | KAL_LOCK_EXCLUSIVE | KAL_LOCK_WAIT)) return kal_err_invalid;
    const bool exclusive = kinds == KAL_LOCK_EXCLUSIVE, wait = mode & KAL_LOCK_WAIT;
#if defined(F_OFD_SETLK)
    struct flock l{}; l.l_type = exclusive ? F_WRLCK : F_RDLCK;
    l.l_whence = SEEK_SET; l.l_start = off_t(start); l.l_len = off_t(len);
    while (fcntl(fd, wait ? F_OFD_SETLKW : F_OFD_SETLK, &l) != 0) {
        if (errno == EINTR) continue;
        return (errno == EACCES || errno == EAGAIN) ? kal_err_again : fail();
    }
    return kal_ok;
#else
    if (start == 0 && len == 0) {
        while (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB)) != 0) {
            if (errno == EINTR) continue;
            return errno == EWOULDBLOCK ? kal_err_again : fail();
        }
        return kal_ok;
    }
    struct flock l{}; l.l_type = exclusive ? F_WRLCK : F_RDLCK;
    l.l_whence = SEEK_SET; l.l_start = off_t(start); l.l_len = off_t(len);
    while (fcntl(fd, wait ? F_SETLKW : F_SETLK, &l) != 0) {
        if (errno == EINTR) continue;
        return (errno == EACCES || errno == EAGAIN) ? kal_err_again : fail();
    }
    return kal_ok;
#endif
}
int kal_fs_unlock(kal_file f, kal_u64 start, kal_u64 len) {
    const int fd = unpack(f.h);
    if (fd < 0) return kal_err_invalid;
#if defined(F_OFD_SETLK)
    struct flock l{}; l.l_type = F_UNLCK; l.l_whence = SEEK_SET; l.l_start = off_t(start); l.l_len = off_t(len);
    return fcntl(fd, F_OFD_SETLK, &l) == 0 ? kal_ok : fail();
#else
    if (start == 0 && len == 0) return flock(fd, LOCK_UN) == 0 ? kal_ok : fail();
    struct flock l{}; l.l_type = F_UNLCK; l.l_whence = SEEK_SET; l.l_start = off_t(start); l.l_len = off_t(len);
    return fcntl(fd, F_SETLK, &l) == 0 ? kal_ok : fail();
#endif
}
int kal_fs_capacity(kal_dir d, kal_u64* total, kal_u64* avail) {
    struct statvfs v;
    if (fstatvfs(unpack(d.h), &v) != 0) return fail();
    if (total) *total = kal_u64(v.f_blocks) * v.f_frsize;
    if (avail) *avail = kal_u64(v.f_bavail) * v.f_frsize;
    return kal_ok;
}
int kal_fs_link_create(kal_dir base, const char* name, kal_uintptr len, const char* target, kal_uintptr tlen, kal_uintptr) {
    cname n(name, len); if (!n.ok || !target || tlen >= PATH_MAX) return kal_err_invalid;
    char t[PATH_MAX]; memcpy(t, target, tlen); t[tlen] = '\0';
    return symlinkat(t, unpack(base.h), n.buf) == 0 ? kal_ok : fail();
}
kal_intptr kal_fs_link_read(kal_dir base, const char* name, kal_uintptr len, char* out, kal_uintptr cap) {
    cname n(name, len); if (!n.ok) return -kal_err_invalid;
    char t[PATH_MAX];
    const ssize_t r = readlinkat(unpack(base.h), n.buf, t, sizeof t);
    if (r < 0) return nfail();
    return put(t, kal_uintptr(r), out, cap);
}
int kal_fs_hard_link(kal_dir from, const char* a, kal_uintptr al, kal_dir to, const char* b, kal_uintptr bl) {
    cname x(a, al), y(b, bl);
    const int f = unpack(from.h), t = unpack(to.h);
    if (f < 0 || t < 0 || !x.ok || !y.ok) return kal_err_invalid;
    // A directory is refused before the call: the C library's answer (EPERM)
    // names a permission. Two volumes are the C library's EXDEV.
    struct stat st;
    if (fstatat(f, x.buf, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)) return kal_err_is_directory;
    if (linkat(f, x.buf, t, y.buf, 0) == 0) return kal_ok;
    if (errno == EXDEV) return kal_err_not_supported;
    return fail();
}
kal_intptr kal_fs_resolved_name(kal_dir, const char*, kal_uintptr, char*, kal_uintptr) { return -kal_err_not_supported; }
int kal_fs_dir_attenuate(kal_dir, const kal_dir_rights*, kal_dir*) { return kal_err_not_supported; }

int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    const int fd = unpack(d.h);
    if (fd < 0 || !iter) return kal_err_invalid;
    const int dup_fd = openat(fd, ".", dir_flags);
    if (dup_fd < 0) return fail();
    DIR* dir = fdopendir(dup_fd);
    if (!dir) { close(dup_fd); return fail(); }
    auto* l = new lister{ dir };
    *iter = reinterpret_cast<kal_uintptr>(l);
    return kal_ok;
}
static int next_entry(int dfd, kal_uintptr* iter, char* name, kal_uintptr cap, kal_uintptr* nlen, int* kind, struct stat* st) {
    if (!iter) return kal_err_invalid;
    auto* l = reinterpret_cast<lister*>(*iter);
    if (!l) { if (nlen) *nlen = 0; return kal_ok; }
    for (;;) {
        errno = 0;
        dirent* e = readdir(l->d);
        if (!e) {
            const int err = errno;
            closedir(l->d); delete l; *iter = 0;
            if (nlen) *nlen = 0;
            return err ? translate(err) : kal_ok;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        const size_t n = strlen(e->d_name);
        put(e->d_name, n, name, cap);
        if (nlen) *nlen = n;
        struct stat s;
        const bool have = fstatat(dfd, e->d_name, &s, AT_SYMLINK_NOFOLLOW) == 0;
        if (kind) *kind = have ? kind_of(s.st_mode) : kal_node_other;
        if (st) { if (have) *st = s; else memset(st, 0, sizeof *st); }
        return kal_ok;
    }
}
int kal_fs_list_next(kal_dir d, kal_uintptr* iter, char* name, kal_uintptr cap, kal_uintptr* nlen, int* kind) {
    if (unpack(d.h) < 0) return kal_err_invalid;
    return next_entry(unpack(d.h), iter, name, cap, nlen, kind, nullptr);
}
int kal_fs_list_next_info(kal_dir d, kal_uintptr* iter, char* name, kal_uintptr cap, kal_uintptr* nlen, kal_u32 wanted, kal_node_info* info) {
    if (unpack(d.h) < 0 || !info) return kal_err_invalid;
    struct stat st; int kind = 0;
    const int r = next_entry(unpack(d.h), iter, name, cap, nlen, &kind, &st);
    if (r == kal_ok && *iter) fill_info(st, info, wanted);
    return r;
}

// ================================================================ process
// HANDLES. A program this one started is its identifier plus one; a program
// found by its identifier carries kOpened, and this program itself kSelf, so
// that the operations a handle admits are told apart: a program does not wait
// for itself, and a program found by its identifier is not the caller's to end.
namespace {
constexpr unsigned kBits = sizeof(kal_uintptr) * 8;
constexpr kal_uintptr kOpened = kal_uintptr(1) << (kBits - 1);
constexpr kal_uintptr kSelf   = kal_uintptr(1) << (kBits - 2);
kal_uintptr started_handle(pid_t pid) { return kal_uintptr(pid) + 1u; }
pid_t pid_of(kal_process p) { return pid_t((p.h & ~(kOpened | kSelf)) - 1u); }

// WHAT AN AWAITED PROGRAM REPORTED, kept while its handle is held: a second
// wait answers the same, and its processor time remains (0.21).
struct ended_program { pid_t pid; int status; int terminated; kal_u64 cpu_ns; kal_u64 peak_bytes; };
constexpr int kEndedMax = 256;
ended_program g_ended[kEndedMax];
int g_ended_count = 0;
pthread_mutex_t g_ended_lock = PTHREAD_MUTEX_INITIALIZER;

bool ended_record(pid_t pid, ended_program* out) {
    pthread_mutex_lock(&g_ended_lock);
    bool found = false;
    for (int i = 0; i < g_ended_count; ++i)
        if (g_ended[i].pid == pid) { if (out) *out = g_ended[i]; found = true; break; }
    pthread_mutex_unlock(&g_ended_lock);
    return found;
}
void ended_forget(pid_t pid) {
    pthread_mutex_lock(&g_ended_lock);
    for (int i = 0; i < g_ended_count; ++i)
        if (g_ended[i].pid == pid) { g_ended[i] = g_ended[--g_ended_count]; break; }
    pthread_mutex_unlock(&g_ended_lock);
}

kal_u64 cpu_of(const struct rusage& ru) {
    return (kal_u64(ru.ru_utime.tv_sec) + kal_u64(ru.ru_stime.tv_sec)) * 1000000000ull
         + (kal_u64(ru.ru_utime.tv_usec) + kal_u64(ru.ru_stime.tv_usec)) * 1000ull;
}
kal_u64 peak_of(const struct rusage& ru) {
#if defined(__APPLE__)
    return kal_u64(ru.ru_maxrss);            // bytes
#else
    return kal_u64(ru.ru_maxrss) * 1024ull;  // kilobytes
#endif
}
}  // namespace

kal_uintptr kal_process_props(void) {
    return KAL_PROCESS_PROP_TERMINATE | KAL_PROCESS_PROP_STREAM_PASSING | KAL_PROCESS_PROP_EXIT_STATUS
         | KAL_PROCESS_PROP_CHANNEL | KAL_PROCESS_PROP_JOB | KAL_PROCESS_PROP_NULL_STREAMS
         | KAL_PROCESS_PROP_OPEN_ID | KAL_PROCESS_PROP_STOP_REQUESTED;
}
int kal_process_spawn(const kal_spawn* how, const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams, kal_process* out) {
    if (!how || !path || !out || path_len == 0 || path_len >= PATH_MAX) return kal_err_invalid;
    if (how->grant_count) return kal_err_not_supported;
    const kal_uintptr unsupported = KAL_SPAWN_BOUND_LIFETIME | KAL_SPAWN_ONE_STRING | KAL_SPAWN_CONFINE;
    if (how->flags & unsupported) return kal_err_not_supported;
    // One preference at most, and the null device and a placed stream are not
    // both at one position.
    const kal_uintptr prefs = how->flags & (KAL_SPAWN_RESOURCES_BALANCED | KAL_SPAWN_RESOURCES_PERFORMANCE
                                            | KAL_SPAWN_RESOURCES_ECONOMY);
    if (prefs & (prefs - 1)) return kal_err_invalid;
    if (streams) {
        const kal_uintptr placed[3] = { streams->in.h, streams->out.h, streams->err.h };
        const kal_uintptr nulls_at[3] = { KAL_SPAWN_NULL_IN, KAL_SPAWN_NULL_OUT, KAL_SPAWN_NULL_ERR };
        for (int i = 0; i < 3; ++i) if (placed[i] && (how->flags & nulls_at[i])) return kal_err_invalid;
    }
    const int base = unpack(how->base.h), work = unpack(how->work.h);
    if (base < 0 || work < 0) return kal_err_invalid;

    // The environment given is examined before anything is allocated or opened.
    for (kal_uintptr i = 0; i < envc; ++i) {
        if (!envp || !envp[i] || !envp_lens) return kal_err_invalid;
        const char* eq = static_cast<const char*>(memchr(envp[i], '=', envp_lens[i]));
        if (!eq || eq == envp[i]) return kal_err_invalid;
    }
    char p[PATH_MAX]; memcpy(p, path, path_len); p[path_len] = '\0';
    // The program is named relative to `base': resolve it to a descriptor now,
    // so that a start that cannot happen is reported by its reason.
    const int exe = openat(base, p, O_RDONLY | O_CLOEXEC);
    if (exe < 0) return fail();
    struct stat st;
    if (fstat(exe, &st) != 0) { close(exe); return fail(); }
    if (S_ISDIR(st.st_mode)) { close(exe); return kal_err_is_directory; }
    if (!(st.st_mode & 0111)) { close(exe); return kal_err_permission; }

    // The null device at the positions that ask for it, opened before the start
    // so that a failure is the caller's to see.
    // A position with no stream placed and no null device keeps the caller's.
    int fds[3] = { -1, -1, -1 };
    const kal_uintptr nulls[3] = { KAL_SPAWN_NULL_IN, KAL_SPAWN_NULL_OUT, KAL_SPAWN_NULL_ERR };
    if (streams) { fds[0] = unpack(streams->in.h); fds[1] = unpack(streams->out.h); fds[2] = unpack(streams->err.h); }
    for (int i = 0; i < 3; ++i) {
        if (!(how->flags & nulls[i])) continue;
        fds[i] = open("/dev/null", (i == 0 ? O_RDONLY : O_WRONLY) | O_CLOEXEC);
        if (fds[i] < 0) {
            const int why = fail();
            for (int k = 0; k < i; ++k) if (how->flags & nulls[k]) close(fds[k]);
            close(exe);
            return why;
        }
    }
    auto release_nulls = [&] { for (int i = 0; i < 3; ++i) if (how->flags & nulls[i]) close(fds[i]); };

    // The descriptor's name in /dev/fd, written before the start: the copy
    // runs in a program that may have other contexts, where only functions
    // safe in a signal handler may be called.
    char self[32];
    {
        char digits[16]; int n = 0; unsigned v = unsigned(exe);
        do { digits[n++] = char('0' + v % 10); v /= 10; } while (v);
        const char prefix[] = "/dev/fd/";
        size_t o = 0;
        for (size_t i = 0; i + 1 < sizeof prefix; ++i) self[o++] = prefix[i];
        while (n) self[o++] = digits[--n];
        self[o] = 0;
    }

    char** av = static_cast<char**>(calloc(argc + 1, sizeof(char*)));
    // THE ENVIRONMENT: the one given, or this program's; and the preference
    // in KAL_RESOURCES where the start states one, in place of the caller's.
    const char* pref = prefs == KAL_SPAWN_RESOURCES_PERFORMANCE ? "KAL_RESOURCES=performance"
                     : prefs == KAL_SPAWN_RESOURCES_ECONOMY     ? "KAL_RESOURCES=economy"
                     : prefs == KAL_SPAWN_RESOURCES_BALANCED    ? "KAL_RESOURCES=balanced" : nullptr;
    kal_uintptr inherited = 0;
    if (envc == 0 && !envp) for (char** e = environ; e && *e; ++e) ++inherited;
    char** ev = static_cast<char**>(calloc(envc + inherited + 2, sizeof(char*)));
    for (kal_uintptr i = 0; i < argc; ++i) av[i] = strndup(argv[i], argv_lens[i]);
    kal_uintptr en = 0;
    for (kal_uintptr i = 0; i < envc; ++i) {
        if (pref && envp_lens[i] >= 14 && memcmp(envp[i], "KAL_RESOURCES=", 14) == 0) continue;
        ev[en++] = strndup(envp[i], envp_lens[i]);
    }
    for (kal_uintptr i = 0; i < inherited; ++i) {
        if (pref && strncmp(environ[i], "KAL_RESOURCES=", 14) == 0) continue;
        ev[en++] = strdup(environ[i]);
    }
    if (pref) ev[en++] = strdup(pref);
    auto release_vectors = [&] {
        for (kal_uintptr i = 0; i < argc; ++i) free(av[i]);
        for (kal_uintptr i = 0; i < en; ++i) free(ev[i]);
        free(av); free(ev);
    };

    const pid_t join = how->job ? pid_t(how->job->h) : 0;  // 0: new unit
    // fork and exec rather than posix_spawn: the working directory is per
    // start, and posix_spawn_file_actions_addfchdir is not on every system.
    int report[2];
    if (pipe(report) != 0) {
        const int why = fail();
        release_vectors(); release_nulls(); close(exe);
        return why;
    }
    fcntl(report[1], F_SETFD, FD_CLOEXEC); fcntl(report[0], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid == 0) {
        if (how->job) setpgid(0, join);
        // A position that cannot be placed ends the copy, and its reason is
        // the caller's; the copy does not keep the caller's descriptor there.
        int saved[3] = { -1, -1, -1 };
        for (int i = 0; i < 3; ++i) {
            if (fds[i] < 0) continue;
            saved[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 10);
            if (saved[i] < 0) { int e = errno; ssize_t w = write(report[1], &e, sizeof e); (void)w; _exit(127); }
        }
        for (int i = 0; i < 3; ++i) {
            if (saved[i] < 0) continue;
            if (dup2(saved[i], i) < 0) { int e = errno; ssize_t w = write(report[1], &e, sizeof e); (void)w; _exit(127); }
        }
        for (int s = 1; s < NSIG; ++s) signal(s, SIG_DFL);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, nullptr);
        if (fchdir(work) != 0) { int e = errno; ssize_t w = write(report[1], &e, sizeof e); (void)w; _exit(127); }
        // THE PROGRAM BY ITS DESCRIPTOR, which names what was resolved relative
        // to `base' before the start: fexecve where the system has it, the
        // name the system keeps for the descriptor where it states one
        // (F_GETPATH), and the descriptor's own name in /dev/fd otherwise.
        // An answer about the program itself ends the attempts: the next way
        // of naming it would only answer about the name.
        auto refused = [&](int why) {
            ssize_t w = write(report[1], &why, sizeof why); (void)w;
            _exit(127);
        };
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__)
        fexecve(exe, av, ev);
        if (errno == ENOEXEC || errno == E2BIG || errno == ENOMEM) refused(errno);
#endif
#if defined(F_GETPATH)
        char named[PATH_MAX];
        if (fcntl(exe, F_GETPATH, named) != -1) {
            execve(named, av, ev);
            refused(errno);
        }
#endif
        execve(self, av, ev);
        int e = errno; ssize_t w = write(report[1], &e, sizeof e); (void)w;
        _exit(127);
    }
    close(report[1]); close(exe);
    release_nulls();
    release_vectors();
    if (pid < 0) { close(report[0]); return fail(); }
    if (how->job) setpgid(pid, join);                   // both sides, as shells do
    int e = 0;
    ssize_t r;
    do r = read(report[0], &e, sizeof e); while (r < 0 && errno == EINTR);
    close(report[0]);
    if (r == sizeof e) { int s; waitpid(pid, &s, 0); return translate(e); }
    if (how->job && how->job->h == 0) how->job->h = kal_uintptr(pid);
    *out = kal_process{ started_handle(pid) };
    return kal_ok;
}
int kal_process_channel(kal_stream* mine, kal_stream* theirs) {
    int p[2];
    if (!mine || !theirs) return kal_err_invalid;
    if (pipe(p) != 0) return fail();
    fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);
    *mine = kal_stream{ pack(p[0]) }; *theirs = kal_stream{ pack(p[1]) };
    return kal_ok;
}
void kal_process_channel_close(kal_stream s) { const int fd = unpack(s.h); if (fd >= 0) close(fd); }
static int reap(pid_t pid, int opts, int* status, int* terminated, bool* done) {
    ended_program e{};
    if (!ended_record(pid, &e)) {
        int s = 0; struct rusage ru{}; pid_t r;
        do r = wait4(pid, &s, opts, &ru); while (r < 0 && errno == EINTR);
        if (r < 0) return fail();
        *done = r != 0;
        if (!*done) return kal_ok;
        e.pid = pid;
        if (WIFEXITED(s)) { e.status = WEXITSTATUS(s); e.terminated = 0; }
        else { e.status = WIFSIGNALED(s) ? 128 + WTERMSIG(s) : 1; e.terminated = 1; }
        e.cpu_ns = cpu_of(ru);
        e.peak_bytes = peak_of(ru);
        pthread_mutex_lock(&g_ended_lock);
        if (g_ended_count < kEndedMax) g_ended[g_ended_count++] = e;
        pthread_mutex_unlock(&g_ended_lock);
    }
    *done = true;
    if (status) *status = e.status;
    if (terminated) *terminated = e.terminated;
    return kal_ok;
}
int kal_process_wait(kal_process p, int* status, int* terminated) {
    if (p.h == 0 || (p.h & kSelf)) return kal_err_invalid;
    if (p.h & kOpened) return kal_err_not_supported;
    bool done = false;
    return reap(pid_of(p), 0, status, terminated, &done);
}
int kal_process_terminate(kal_process p) {
    if (p.h == 0 || (p.h & kSelf)) return kal_err_invalid;
    if (p.h & kOpened) return kal_err_not_supported;
    // A REQUEST TO END, as the other implementations send it: SIGTERM, which
    // the program may take over (kal_process_stop_requested).
    if (ended_record(pid_of(p), nullptr)) return kal_ok;
    return kill(pid_of(p), SIGTERM) == 0 ? kal_ok : fail();
}
void kal_process_close(kal_process p) {
    if (p.h != 0 && !(p.h & (kOpened | kSelf))) ended_forget(pid_of(p));
}
int kal_process_self(kal_process* out) {
    if (!out) return kal_err_invalid;
    *out = kal_process{ started_handle(getpid()) | kSelf };
    return kal_ok;
}
int kal_process_open_id(kal_u64 id, kal_process* out) {
    if (!out || id == 0 || id > kal_u64(INT32_MAX)) return kal_err_invalid;
    if (kill(pid_t(id), 0) != 0 && errno == ESRCH) return kal_err_not_found;
    *out = kal_process{ started_handle(pid_t(id)) | kOpened };
    return kal_ok;
}
int kal_process_info(kal_process p, kal_u32 wanted, kal_program_info* out) {
    if (!out || out->self_size < 8 || p.h == 0) return kal_err_invalid;
    const kal_u32 cap = out->self_size;
    kal_program_info i{}; i.self_size = cap;
    const pid_t pid = pid_of(p);
    i.present = KAL_PINFO_ID; i.id = kal_u64(pid);
    if (p.h & kSelf) {
        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            i.cpu_ns = cpu_of(ru); i.memory_peak_bytes = peak_of(ru);
            i.present |= KAL_PINFO_CPU;
        }
        i.present |= KAL_PINFO_ENDED; i.ended = 0;
    } else if (!(p.h & kOpened)) {
        ended_program e{};
        i.present |= KAL_PINFO_ENDED;
        if (ended_record(pid, &e)) { i.ended = 1; i.cpu_ns = e.cpu_ns; i.present |= KAL_PINFO_CPU; }
        else i.ended = 0;
    }
    i.present &= wanted | KAL_PINFO_ID;
    struct { kal_u32 bit; size_t end; } const fields[] = {
        { KAL_PINFO_ID,    offsetof(kal_program_info, id) + 8 },
        { KAL_PINFO_CPU,   offsetof(kal_program_info, cpu_ns) + 8 },
        { KAL_PINFO_ENDED, offsetof(kal_program_info, ended) + 4 },
    };
    for (const auto& f : fields) if (cap < f.end) i.present &= ~f.bit;
    memcpy(out, &i, cap < sizeof i ? cap : sizeof i);
    return kal_ok;
}
int kal_process_job_enter(kal_job* j) {
    if (!j) return kal_err_invalid;
    if (j->h == 0) { if (setpgid(0, 0) != 0) return fail(); j->h = kal_uintptr(getpid()); return kal_ok; }
    return setpgid(0, pid_t(j->h)) == 0 ? kal_ok : fail();
}
int kal_process_job_terminate(kal_job j) { return (j.h && killpg(pid_t(j.h), SIGKILL) == 0) ? kal_ok : fail(); }
void kal_process_job_close(kal_job) {}
// Declared units, usage and limits are not claimed (KAL_PROCESS_PROP_JOB_OBSERVE,
// _JOB_USAGE, _JOB_LIMIT_*), and the operations report so (clause 6.2).
int kal_process_job_create(kal_job*, char*, kal_uintptr, kal_uintptr*) { return kal_err_not_supported; }
int kal_process_job_open(const char*, kal_uintptr, kal_job*) { return kal_err_not_supported; }
int kal_process_job_observe(kal_job, const char*, kal_uintptr, kal_u32*) { return kal_err_not_supported; }
int kal_process_job_retire(kal_job, const char*, kal_uintptr, const kal_program_identity*, kal_u32*) { return kal_err_not_supported; }
int kal_process_job_usage(kal_job, kal_job_usage*) { return kal_err_not_supported; }
int kal_process_job_limit(kal_job, kal_uintptr kind, kal_u64) {
    return kind == KAL_JOB_LIMIT_MEMORY ? kal_err_not_supported : kal_err_invalid;
}

// ---------------------------------------------------------------- the stop word
// THE REQUESTS TO END, TAKEN OVER. SIGINT, SIGTERM and SIGHUP set the word
// (counting each, and never zero again once one has arrived); a request the
// program's starter arranged to ignore stays ignored. A handler may not call
// the threads library, so it writes a byte to a pipe, and a context created
// when the word is armed reads it and wakes the word's waiters.
namespace {
kal_u32 g_stop_word = 0;
int g_stop_signal = 0;
int g_stop_pipe[2] = { -1, -1 };
pthread_once_t g_stop_once = PTHREAD_ONCE_INIT;
const int g_stop_signals[] = { SIGINT, SIGTERM, SIGHUP };

void stop_handler(int signo) {
    const int saved = errno;
    int none = 0;
    __atomic_compare_exchange_n(&g_stop_signal, &none, signo, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    kal_u32 seen = __atomic_load_n(&g_stop_word, __ATOMIC_RELAXED), next;
    do {
        next = seen + 1u;
        if (next == 0) next = 1u;
    } while (!__atomic_compare_exchange_n(&g_stop_word, &seen, next, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    const char b = 1;
    ssize_t w = write(g_stop_pipe[1], &b, 1); (void)w;
    errno = saved;
}

void* stop_waker(void*) {
    char b;
    for (;;) {
        const ssize_t r = read(g_stop_pipe[0], &b, 1);
        if (r == 1) kal_task_wake(&g_stop_word, ~kal_uintptr{0}, nullptr);
        else if (r < 0 && errno == EINTR) continue;
        else return nullptr;
    }
}

void arm_stop() {
    if (pipe(g_stop_pipe) != 0) { g_stop_pipe[0] = g_stop_pipe[1] = -1; return; }
    fcntl(g_stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_stop_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_stop_pipe[1], F_SETFL, O_NONBLOCK);
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    const int made = pthread_create(&t, &a, stop_waker, nullptr);
    pthread_attr_destroy(&a);
    // WITHOUT THE CONTEXT THAT WAKES THE WORD'S WAITERS THERE IS NO WORD: a
    // word whose waiters could not be woken is not answered, and no handler
    // is installed.
    if (made != 0) {
        close(g_stop_pipe[0]); close(g_stop_pipe[1]);
        g_stop_pipe[0] = g_stop_pipe[1] = -1;
        return;
    }
    for (const int sig : g_stop_signals) {
        struct sigaction old{};
        if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler == SIG_IGN) continue;
        struct sigaction act{};
        act.sa_handler = stop_handler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_RESTART;
        sigaction(sig, &act, nullptr);
    }
}
}  // namespace

const kal_u32* kal_process_stop_requested(void) {
    pthread_once(&g_stop_once, arm_stop);
    return g_stop_pipe[1] >= 0 ? &g_stop_word : nullptr;
}

int kal_process_end_as_requested(void) {
    const int sig = __atomic_load_n(&g_stop_signal, __ATOMIC_ACQUIRE);
    if (sig == 0 || __atomic_load_n(&g_stop_word, __ATOMIC_ACQUIRE) == 0) return kal_err_invalid;
    // The request's own disposition, restored, and the request made again:
    // the starter observes the program ended by it.
    struct sigaction act{};
    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    sigaction(sig, &act, nullptr);
    sigset_t one; sigemptyset(&one); sigaddset(&one, sig);
    pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
    raise(sig);
    return kal_err_invalid;
}

// ================================================================ task
namespace {
struct start_box { void (*entry)(void*); void* arg; };
void* trampoline(void* p) { start_box b = *static_cast<start_box*>(p); free(p); b.entry(b.arg); return nullptr; }

// Waiting upon a word, composed from mutexes and condition variables so that
// it needs nothing but POSIX threads: a word hashes to a bucket, and a waker
// broadcasts the bucket. A woken waiter that finds the word unchanged has been
// woken spuriously, which the operation permits.
struct bucket { pthread_mutex_t m; pthread_cond_t c; unsigned waiters; };
bucket g_buckets[64];
pthread_once_t g_buckets_once = PTHREAD_ONCE_INIT;
void init_buckets() {
    for (auto& b : g_buckets) {
        pthread_mutex_init(&b.m, nullptr);
        pthread_condattr_t a; pthread_condattr_init(&a);
#if !defined(__APPLE__)
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
        pthread_cond_init(&b.c, &a);
        b.waiters = 0;
    }
}
bucket* bucket_of(const kal_u32* w) {
    pthread_once(&g_buckets_once, init_buckets);
    return &g_buckets[(reinterpret_cast<kal_uintptr>(w) >> 2) % 64];
}
}  // namespace

int kal_task_start_sized(void (*entry)(void*), void* arg, kal_uintptr size, kal_task* out) {
    if (!entry || !out) return kal_err_invalid;
    auto* b = static_cast<start_box*>(malloc(sizeof(start_box)));
    if (!b) return kal_err_no_memory;
    b->entry = entry; b->arg = arg;
    pthread_attr_t a; pthread_attr_init(&a);
    kal_uintptr want = size < (8u << 20) ? (8u << 20) : size;
    pthread_attr_setstacksize(&a, want);
    pthread_t t;
    const int r = pthread_create(&t, &a, trampoline, b);
    pthread_attr_destroy(&a);
    if (r != 0) { free(b); return r == ENOMEM || r == EAGAIN ? kal_err_no_memory : translate(r); }
    *out = kal_task{ reinterpret_cast<kal_uintptr>(t) };
    return kal_ok;
}
int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) { return kal_task_start_sized(entry, arg, 0, out); }
int kal_task_join(kal_task t) { return pthread_join(reinterpret_cast<pthread_t>(t.h), nullptr) == 0 ? kal_ok : kal_err_invalid; }
void kal_task_yield(void) { sched_yield(); }
kal_uintptr kal_task_current(void) { return reinterpret_cast<kal_uintptr>(pthread_self()); }
int kal_task_stack(void** base, kal_uintptr* size) {
    if (!base || !size) return kal_err_invalid;
    void* addr = nullptr; size_t sz = 0;
#if defined(__linux__)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return kal_err_io;
    pthread_attr_getstack(&a, &addr, &sz); pthread_attr_destroy(&a);
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
    pthread_attr_t a; pthread_attr_init(&a);
    if (pthread_attr_get_np(pthread_self(), &a) != 0) return kal_err_io;
    pthread_attr_getstack(&a, &addr, &sz); pthread_attr_destroy(&a);
#elif defined(__OpenBSD__)
    stack_t s; if (pthread_stackseg_np(pthread_self(), &s) != 0) return kal_err_io;
    sz = s.ss_size; addr = static_cast<char*>(s.ss_sp) - sz;
#elif defined(__APPLE__)
    sz = pthread_get_stacksize_np(pthread_self());
    addr = static_cast<char*>(pthread_get_stackaddr_np(pthread_self())) - sz;
#else
    return kal_err_io;
#endif
    *base = addr; *size = sz;
    return kal_ok;
}
kal_uintptr kal_task_parallelism(void) { const long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? kal_uintptr(n) : 0; }
kal_uintptr kal_task_cores(void) { return 0; }
int kal_task_set_priority(kal_uintptr pr) {
    if (pr > KAL_TASK_PRIORITY_BACKGROUND) return kal_err_invalid;
    const int nice_v = pr == KAL_TASK_PRIORITY_DEFAULT ? 0 : pr == KAL_TASK_PRIORITY_LOW ? 10 : 19;
    return setpriority(PRIO_PROCESS, 0, nice_v) == 0 ? kal_ok : fail();
}
int kal_task_wait(const kal_u32* word, kal_u32 expected, kal_u64 timeout_ns) {
    if (!word) return kal_err_invalid;
    bucket& b = *bucket_of(word);
    pthread_mutex_lock(&b.m);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) { pthread_mutex_unlock(&b.m); return kal_ok; }  // changed already: re-examine
    ++b.waiters;
    int r = 0;
    if (timeout_ns == 0) r = pthread_cond_wait(&b.c, &b.m);
    else {
#if defined(__APPLE__)
        // This system's condition variables measure a bound relative to now.
        timespec t{ time_t(timeout_ns / 1000000000ull), long(timeout_ns % 1000000000ull) };
        r = pthread_cond_timedwait_relative_np(&b.c, &b.m, &t);
#else
        // The bound on the monotonic clock the condition variable was made with.
        timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        const kal_u64 end = kal_u64(t.tv_sec) * 1000000000ull + kal_u64(t.tv_nsec) + timeout_ns;
        t.tv_sec = time_t(end / 1000000000ull); t.tv_nsec = long(end % 1000000000ull);
        r = pthread_cond_timedwait(&b.c, &b.m, &t);
#endif
    }
    --b.waiters;
    pthread_mutex_unlock(&b.m);
    return r == ETIMEDOUT ? kal_err_again : kal_ok;
}
int kal_task_wake(const kal_u32* word, kal_uintptr count, kal_uintptr* woken) {
    if (!word) return kal_err_invalid;
    bucket& b = *bucket_of(word);
    pthread_mutex_lock(&b.m);
    const unsigned n = b.waiters;
    if (count && n) pthread_cond_broadcast(&b.c);
    pthread_mutex_unlock(&b.m);
    if (woken) *woken = count < n ? count : n;
    return kal_ok;
}
kal_uintptr kal_task_props(void) {
    return KAL_TASK_PROP_PREEMPTIVE | KAL_TASK_PROP_PARALLEL | KAL_TASK_PROP_WAIT_TIMEOUT
         | KAL_TASK_PROP_THREAD_LOCAL | KAL_TASK_PROP_PRIORITY;
}

// ================================================================ exec
// Memory a loader writes and then executes. The region is never writable and
// executable at once (W^X): it is reserved writable and published read+execute.
kal_uintptr kal_exec_granularity(void) { return kal_uintptr(sysconf(_SC_PAGESIZE)); }
static int rw_prot() {
#if defined(__NetBSD__) && defined(PROT_MPROTECT)
    // PaX MPROTECT: a mapping may only later gain what it was created allowing.
    return PROT_READ | PROT_WRITE | PROT_MPROTECT(PROT_READ | PROT_WRITE | PROT_EXEC);
#else
    return PROT_READ | PROT_WRITE;
#endif
}
void* kal_exec_alloc(kal_uintptr size) {
    void* p = mmap(nullptr, size, rw_prot(), MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void* kal_exec_alloc_at(void* at, kal_uintptr size) {
    if (!at || !size || reinterpret_cast<kal_uintptr>(at) % kal_memory_granularity()) return nullptr;
#if !defined(MAP_FIXED_NOREPLACE) && !defined(MAP_EXCL)
    // No mapping at an address that refuses what is already there, so
    // KAL_EXEC_PROP_PLACE is not claimed and a region at an address is refused.
    return nullptr;
#endif
    int fl = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_FIXED_NOREPLACE)
    fl |= MAP_FIXED_NOREPLACE;
#elif defined(MAP_EXCL)
    fl |= MAP_FIXED | MAP_EXCL;
#endif
    void* p = mmap(at, size, rw_prot(), fl, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (p != at) { munmap(p, size); return nullptr; }
    return p;
}
int kal_exec_publish(void* p, kal_uintptr size) { return mprotect(p, size, PROT_READ | PROT_EXEC) == 0 ? kal_ok : fail(); }
int kal_exec_publish_part(void* r, kal_uintptr off, kal_uintptr size) {
    if (off % kal_exec_granularity() || size % kal_exec_granularity()) return kal_err_invalid;
    return mprotect(static_cast<char*>(r) + off, size, PROT_READ | PROT_EXEC) == 0 ? kal_ok : fail();
}
int kal_exec_seal_part(void* r, kal_uintptr off, kal_uintptr size, kal_uintptr flags) {
    if (flags & ~KAL_EXEC_SEAL_FOREVER) return kal_err_invalid;
    if (off % kal_exec_granularity() || size % kal_exec_granularity()) return kal_err_invalid;
    return mprotect(static_cast<char*>(r) + off, size, PROT_READ) == 0 ? kal_ok : fail();
}
int kal_exec_map_part(void* r, kal_uintptr off, kal_uintptr size, kal_file f, kal_u64 at, kal_uintptr how) {
    const kal_uintptr g = kal_exec_granularity();
    if (!r || size == 0 || off % g || size % g || at % KAL_MAP_ALIGNMENT) return kal_err_invalid;
    if (how & ~(KAL_EXEC_MAP_EXECUTE | KAL_EXEC_MAP_PREFETCH | KAL_EXEC_MAP_BTI | KAL_EXEC_MAP_WRITE))
        return kal_err_invalid;
    if ((how & KAL_EXEC_MAP_WRITE) && (how & KAL_EXEC_MAP_EXECUTE)) return kal_err_invalid;
    struct stat st;
    if (fstat(unpack(f.h), &st) != 0) return fail();
    if (at + size > kal_u64(st.st_size)) return kal_err_invalid;
    int prot = PROT_READ;
    if (how & KAL_EXEC_MAP_EXECUTE) prot |= PROT_EXEC;
    if (how & KAL_EXEC_MAP_WRITE)   prot |= PROT_WRITE;
    // BTI is not enforced by this implementation (KAL_EXEC_PROP_BTI is not
    // claimed), and the part is mapped as without it.
    void* p = mmap(static_cast<char*>(r) + off, size, prot, MAP_PRIVATE | MAP_FIXED, unpack(f.h), off_t(at));
    if (p == MAP_FAILED) return fail();
    // Read in now, where asked: a request about time, which the system may
    // decline without changing what the part holds.
    if (how & KAL_EXEC_MAP_PREFETCH) posix_madvise(p, size, POSIX_MADV_WILLNEED);
    return kal_ok;
}
void kal_exec_free(void* p, kal_uintptr size) { if (p) munmap(p, size); }
kal_uintptr kal_exec_props(void) {
    kal_uintptr p = KAL_EXEC_PROP_AVAILABLE | KAL_EXEC_PROP_FILE | KAL_EXEC_PROP_REPUBLISH;
#if defined(MAP_FIXED_NOREPLACE) || defined(MAP_EXCL)
    p |= KAL_EXEC_PROP_PLACE;
#endif
    return p;
}

// ================================================================ terminal
int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    termios t; if (!mode) return kal_err_invalid;
    if (tcgetattr(unpack(s.h), &t) != 0) return errno == ENOTTY ? kal_err_not_supported : fail();
    kal_uintptr m = KAL_TERM_SEQUENCES;
    if (t.c_lflag & ICANON) m |= KAL_TERM_LINE_EDIT;
    if (t.c_lflag & ECHO)   m |= KAL_TERM_ECHO;
    if (!(t.c_lflag & ISIG) && !(t.c_iflag & IXON)) m |= KAL_TERM_PASS_CONTROL;
    if (!(t.c_iflag & (ICRNL | INLCR | IGNCR | ISTRIP))) m |= KAL_TERM_PASS_INPUT;
    *mode = m;
    return kal_ok;
}
int kal_terminal_set_mode(kal_stream s, kal_uintptr m) {
    termios t; const int fd = unpack(s.h);
    if (tcgetattr(fd, &t) != 0) return errno == ENOTTY ? kal_err_not_supported : fail();
    auto setb = [](tcflag_t& w, tcflag_t b, bool on) { w = on ? (w | b) : (w & ~b); };
    setb(t.c_lflag, ICANON, m & KAL_TERM_LINE_EDIT);
    setb(t.c_lflag, ECHO,   m & KAL_TERM_ECHO);
    setb(t.c_lflag, ISIG | IEXTEN, !(m & KAL_TERM_PASS_CONTROL));
    setb(t.c_iflag, IXON,   !(m & KAL_TERM_PASS_CONTROL));
    setb(t.c_iflag, ICRNL | INLCR | IGNCR | ISTRIP, !(m & KAL_TERM_PASS_INPUT));
    if (!(m & KAL_TERM_LINE_EDIT)) { t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; }
    return tcsetattr(fd, TCSAFLUSH, &t) == 0 ? kal_ok : fail();
}
int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
    winsize w{};
    if (ioctl(unpack(s.h), TIOCGWINSZ, &w) != 0 || w.ws_col == 0) return kal_err_not_supported;
    if (cols) *cols = w.ws_col;
    if (rows) *rows = w.ws_row;
    return kal_ok;
}
kal_uintptr kal_terminal_props(kal_stream s) {
    const int fd = unpack(s.h);
    return (fd >= 0 && isatty(fd)) ? (KAL_TERM_PROP_MODE | KAL_TERM_PROP_SIZE) : 0;
}

// ================================================================ timeout
static int ready(int fd, short ev, kal_u64 ns) {
    pollfd p{ fd, ev, 0 };
    const int ms = ns == 0 ? -1 : int((ns + 999999u) / 1000000u);
    for (;;) {
        const int r = poll(&p, 1, ms);
        if (r > 0) return kal_ok;
        if (r == 0) return kal_err_again;
        if (errno != EINTR) return fail();
    }
}
kal_intptr kal_timeout_read(kal_stream s, void* buf, kal_uintptr len, kal_u64 ns) {
    const int r = ready(unpack(s.h), POLLIN, ns);
    if (r != kal_ok) return -kal_intptr(r);
    return kal_stream_read(s, buf, len);
}
kal_intptr kal_timeout_write(kal_stream s, const void* buf, kal_uintptr len, kal_u64 ns) {
    const int r = ready(unpack(s.h), POLLOUT, ns);
    if (r != kal_ok) return -kal_intptr(r);
    return kal_stream_write(s, buf, len);
}
int kal_timeout_wait_process(kal_process p, kal_u64 ns, int* status, int* terminated) {
    if (p.h == 0 || (p.h & kSelf)) return kal_err_invalid;
    if (p.h & kOpened) return kal_err_not_supported;
    const kal_u64 end = kal_time_monotonic() + ns;
    for (;;) {
        bool done = false;
        const int r = reap(pid_of(p), WNOHANG, status, terminated, &done);
        if (r != kal_ok || done) return r;
        if (ns != 0 && kal_time_monotonic() >= end) return kal_err_again;
        kal_time_sleep(1000000);   // one millisecond, as openkal-musl documents for Linux
    }
}
kal_u64 kal_timeout_granularity(void) { return 1000000; }
// The network's resources are not provided (openkal.net and openkal.datagram
// are absent), and timeout.h states that the operations of a resource type an
// implementation withholds report kal_err_not_supported.
int kal_timeout_accept(kal_net_listener, kal_u64, kal_net_conn*) { return kal_err_not_supported; }
kal_intptr kal_timeout_recv_from(kal_datagram, void*, kal_uintptr, kal_endpoint*, kal_u64) {
    return -kal_err_not_supported;
}


// ================================================================ region
// Address space reserved without backing, backed in parts (an allocator's).
void* kal_region_reserve(kal_uintptr size, kal_uintptr align, kal_uintptr flags) {
    if (flags & ~KAL_REGION_COMMIT) return nullptr;
    const kal_uintptr g = kal_memory_granularity();
    if (size == 0 || size % g) return nullptr;
    if (align < g) align = g;
    if (align & (align - 1)) return nullptr;
    const int prot = (flags & KAL_REGION_COMMIT) ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    const kal_uintptr span = size + (align > g ? align : 0);
    char* p = static_cast<char*>(mmap(nullptr, span, prot, MAP_PRIVATE | MAP_ANON
#if defined(MAP_NORESERVE)
                                      | MAP_NORESERVE
#endif
                                      , -1, 0));
    if (p == MAP_FAILED) return nullptr;
    if (span == size) return p;
    char* a = reinterpret_cast<char*>((reinterpret_cast<kal_uintptr>(p) + align - 1) & ~(align - 1));
    if (a > p) munmap(p, kal_uintptr(a - p));
    if (a + size < p + span) munmap(a + size, kal_uintptr(p + span - (a + size)));
    return a;
}
int kal_region_commit(void* addr, kal_uintptr len) {
    return mprotect(addr, len, PROT_READ | PROT_WRITE) == 0 ? kal_ok : fail();
}
int kal_region_decommit(void* addr, kal_uintptr len) {
    // Replaced by fresh reserved pages: the contents are gone and the backing
    // is returned.
    void* p = mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? fail() : kal_ok;
}
int kal_region_discard(void* addr, kal_uintptr len) {
    // The contents need not be kept; the part stays backed.
    return posix_madvise(addr, len, POSIX_MADV_DONTNEED) == 0 ? kal_ok : kal_ok;
}
void kal_region_release(void* addr, kal_uintptr size) { if (addr) munmap(addr, size); }
kal_uintptr kal_region_props(void) { return KAL_REGION_PROP_OVERCOMMIT; }

// ================================================================ map
int kal_map_file(kal_file f, kal_u64 offset, kal_uintptr length, kal_uintptr how, void** out) {
    if (!out || length == 0 || offset % KAL_MAP_ALIGNMENT || (how & ~(KAL_MAP_WRITE | KAL_MAP_SHARED))) return kal_err_invalid;
    struct stat st;
    if (fstat(unpack(f.h), &st) != 0) return fail();
    if (offset + length > kal_u64(st.st_size)) return kal_err_invalid;
    const int prot = PROT_READ | ((how & KAL_MAP_WRITE) ? PROT_WRITE : 0);
    void* p = mmap(nullptr, length, prot, (how & KAL_MAP_SHARED) ? MAP_SHARED : MAP_PRIVATE, unpack(f.h), off_t(offset));
    if (p == MAP_FAILED) return errno == ENODEV ? kal_err_not_supported : fail();
    *out = p;
    return kal_ok;
}
int kal_map_sync(void* region, kal_uintptr length) { return msync(region, length, MS_SYNC) == 0 ? kal_ok : fail(); }
void kal_map_release(void* region, kal_uintptr length) { if (region) munmap(region, length); }


// ================================================================ space
// A context in a copy of the calling address space: the host's fork. The
// started context runs `entry' on the copy of the caller's stack; the
// specification lets an implementation whose environment gives the copy a
// stack of its own ignore `stack_top'. Descriptors are copied with it.
int kal_space_start(void (*entry)(void*), void* arg, void*, kal_process* out) {
    if (!entry || !out) return kal_err_invalid;
    const pid_t pid = fork();
    if (pid < 0) return fail();
    if (pid == 0) { entry(arg); _exit(0); }
    *out = kal_process{ started_handle(pid) };
    return kal_ok;
}
kal_uintptr kal_space_props(void) { return KAL_SPACE_PROP_CLONE_HANDLES; }

}  // extern "C"
