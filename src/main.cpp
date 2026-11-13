#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/logger.hpp"
#include "core/version.hpp"
#include <exception>
#include <csignal>
#include <cstdlib>
#include "core/env.hpp"
#include <cctype>
#include <filesystem>

#include "core/data_paths.hpp"
#include <string>
#include <SDL3/SDL.h>
// SDLActivity loads libwowee.so and calls SDL_main, the name this header gives
// main(). SDL2's SDL.h pulled it in; SDL3's does not, and without it the
// library exports only main and the activity has nothing to call.
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// backtrace(3) and friends live in libSystem on macOS and in glibc on Linux, so
// the useful crash report - faulting address, symbolized frames, a copy in the
// crash log - works the same on both. It used to be guarded on __linux__ alone,
// which left a macOS crash with no backtrace and no log at all. Only the X11
// mouse ungrab below is genuinely Linux-specific.
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <libgen.h>
#include <cstdio>
#include <cstring>
// Bionic ships execinfo.h but declares backtrace() only from API 33, and this
// build targets lower. The crash log keeps everything but its stack section on
// Android, where logcat carries a native trace anyway.
#if !defined(__ANDROID__)
#define WOWEE_HAS_BACKTRACE 1
#include <execinfo.h>
#include <fcntl.h>
#include <setjmp.h>
#include <ucontext.h>
#include <iterator>
#endif
#endif

// Android defines __linux__ and has no X11, so the mouse-ungrab path is gated
// on both. The other Linux branches in this file - /proc/self/exe, the
// backtrace - are correct there and are left alone.
#if defined(__linux__) && !defined(__ANDROID__)
#include <X11/Xlib.h>

// Keep a persistent X11 connection for emergency mouse release in signal handlers.
// XOpenDisplay inside a signal handler is unreliable, so we open it once at startup.
static Display* g_emergencyDisplay = nullptr;

static void releaseMouseGrab() {
    if (g_emergencyDisplay) {
        XUngrabPointer(g_emergencyDisplay, CurrentTime);
        XUngrabKeyboard(g_emergencyDisplay, CurrentTime);
        XFlush(g_emergencyDisplay);
    }
}
#else
static void releaseMouseGrab() {}
#endif

#ifdef WOWEE_HAS_BACKTRACE
// The crash report, written from inside the signal handler with nothing but
// async-signal-safe calls: write(2), open(2), and backtrace_symbols_fd, which
// writes straight to a descriptor without allocating.
//
// It used to call fprintf and fopen, and backtrace() cold. The first
// backtrace() of a process loads libgcc_s, which allocates - and a crash in
// the Vulkan driver took the handler down inside backtrace(), so no stack was
// written at all. backtrace() is now called once at start-up
// (installCrashHandler), the handler runs on its own stack, and a fault while
// unwinding jumps back out, so the report still gives the faulting
// instruction and whatever frames were found instead of a second crash.
namespace {
volatile sig_atomic_t g_inCrashHandler = 0;
volatile sig_atomic_t g_unwinding = 0;
sigjmp_buf g_unwindJump;
void* g_crashFrames[128];
alignas(16) char g_crashStack[64 * 1024];

void crashWrite(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = 0;
    while (s[len]) ++len;
    while (len > 0) {
        const ssize_t w = write(fd, s, len);
        if (w <= 0) return;
        s += w;
        len -= static_cast<size_t>(w);
    }
}

/// Hex without printf, which is not async-signal-safe.
void crashWriteHex(int fd, uintptr_t v) {
    constexpr int kDigits = static_cast<int>(sizeof(uintptr_t) * 2);
    char buf[2 + kDigits + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < kDigits; ++i) {
        const unsigned nib = static_cast<unsigned>((v >> ((kDigits - 1 - i) * 4)) & 0xfu);
        buf[2 + i] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    buf[2 + kDigits] = '\0';
    crashWrite(fd, buf);
}

void crashWriteInt(int fd, int v) {
    char buf[16];
    int i = static_cast<int>(sizeof(buf)) - 1;
    buf[i] = '\0';
    unsigned u = v < 0 ? 0u - static_cast<unsigned>(v) : static_cast<unsigned>(v);
    do { buf[--i] = static_cast<char>('0' + u % 10); u /= 10; } while (u && i > 1);
    if (v < 0) buf[--i] = '-';
    crashWrite(fd, buf + i);
}

uintptr_t crashProgramCounter(void* ucontext) {
#if defined(__linux__) && defined(__x86_64__)
    if (ucontext) return static_cast<uintptr_t>(static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    if (ucontext) return static_cast<uintptr_t>(static_cast<ucontext_t*>(ucontext)->uc_mcontext.pc);
#else
    (void)ucontext;
#endif
    return 0;
}

void crashReport(int fd, int sig, void* faultAddr, uintptr_t pc, int frameCount, bool unwindFaulted) {
    if (fd < 0) return;
    const char* sigName = (sig == SIGSEGV) ? "SIGSEGV" :
                          (sig == SIGBUS)  ? "SIGBUS"  :
                          (sig == SIGILL)  ? "SIGILL"  :
                          (sig == SIGABRT) ? "SIGABRT" :
                          (sig == SIGFPE)  ? "SIGFPE"  : "UNKNOWN";
    crashWrite(fd, "\n=== CRASH: signal ");
    crashWrite(fd, sigName);
    crashWrite(fd, " (");
    crashWriteInt(fd, sig);
    crashWrite(fd, ") faultAddr=");
    crashWriteHex(fd, reinterpret_cast<uintptr_t>(faultAddr));
    crashWrite(fd, " pc=");
    crashWriteHex(fd, pc);
    crashWrite(fd, " ===\n");
    // The faulting instruction first, as module+offset: the one frame there
    // is even when nothing under it can be unwound.
    if (pc) {
        void* at = reinterpret_cast<void*>(pc);
        crashWrite(fd, "faulting instruction:\n");
        backtrace_symbols_fd(&at, 1, fd);
    }
    if (unwindFaulted) crashWrite(fd, "(the unwinder faulted; the stack below is as far as it got)\n");
    if (frameCount > 0) {
        crashWrite(fd, "stack:\n");
        backtrace_symbols_fd(g_crashFrames, frameCount, fd);
    } else {
        crashWrite(fd, "stack: none could be unwound\n");
    }
}

/// The stack, into g_crashFrames. In a function of its own, with nothing
/// live across the sigsetjmp but statics: the handler's arguments are not
/// safe to read after a siglongjmp back into its frame.
[[gnu::noinline]] int crashCollectFrames(bool& faulted) {
    static volatile int count = 0;
    static volatile bool jumped = false;
    if (sigsetjmp(g_unwindJump, 1) == 0) {
        g_unwinding = 1;
        count = backtrace(g_crashFrames, static_cast<int>(std::size(g_crashFrames)));
        g_unwinding = 0;
    } else {
        // backtrace() fills the buffer as it walks, so the frames found
        // before the fault are there.
        jumped = true;
        int n = 0;
        while (n < static_cast<int>(std::size(g_crashFrames)) && g_crashFrames[n]) ++n;
        count = n;
    }
    faulted = jumped;
    return count;
}

void crashHandlerSigaction(int sig, siginfo_t* info, void* ucontext) {
    // A fault inside backtrace() below lands here again (SA_NODEFER); the
    // walk is abandoned and the report goes on with what it had found.
    if (g_unwinding) {
        g_unwinding = 0;
        siglongjmp(g_unwindJump, 1);
    }
    // Any other fault while reporting: give up on the report and die of the
    // signal rather than loop.
    if (g_inCrashHandler) {
        signal(sig, SIG_DFL);
        raise(sig);
        _exit(128 + sig);
    }
    g_inCrashHandler = 1;

    void* faultAddr = info ? info->si_addr : nullptr;
    const uintptr_t pc = crashProgramCounter(ucontext);

    bool unwindFaulted = false;
    const int frameCount = crashCollectFrames(unwindFaulted);

    crashReport(STDERR_FILENO, sig, faultAddr, pc, frameCount, unwindFaulted);
    const int debugFd = open("/tmp/wowee_debug.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    crashReport(debugFd, sig, faultAddr, pc, frameCount, unwindFaulted);
    if (debugFd >= 0) close(debugFd);
    // The report is on disk; what follows is not async-signal-safe and is
    // tried only now. A fault in it dies of the signal (g_inCrashHandler).
    releaseMouseGrab();
    // The lines the logger was still holding, before the report goes after
    // them: a second or two of the log sits in its buffer, and the crash of
    // 2026-11-13 lost the rebuild that led to it that way. Not if the lock
    // is held - the fault may have come from inside the logger.
    wowee::core::Logger::getInstance().flushForCrash();
    // And the session's own log, the file a report comes with.
    if (const char* logPath = wowee::core::Logger::filePathForCrashReport()) {
        const int logFd = open(logPath, O_WRONLY | O_APPEND | O_CLOEXEC);
        crashReport(logFd, sig, faultAddr, pc, frameCount, unwindFaulted);
        if (logFd >= 0) close(logFd);
    }

    // Die of the signal with the default action, so the core is still made.
    signal(sig, SIG_DFL);
    raise(sig);
}

void installCrashHandler() {
    // backtrace() loads libgcc_s and allocates the first time it runs; done
    // here, so the handler's call is only a walk of the stack.
    {
        void* warm[2];
        (void)backtrace(warm, 2);
    }
    // Its own stack, so an overflow of the main one can still be reported.
    stack_t ss{};
    ss.ss_sp = g_crashStack;
    ss.ss_size = sizeof(g_crashStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashHandlerSigaction;
    // SA_NODEFER: a fault inside the handler has to reach it again, to jump
    // out of the unwinder. Held blocked instead, a recurring SIGSEGV kills
    // the process on the spot with nothing written.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}
}  // namespace
#else
static void crashHandler(int sig) {
    releaseMouseGrab();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

static wowee::core::LogLevel readLogLevelFromEnv() {
    const char* raw = std::getenv("WOWEE_LOG_LEVEL");
    if (!raw || !*raw) return wowee::core::LogLevel::WARNING;
    std::string level(raw);
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (level == "debug") return wowee::core::LogLevel::DEBUG;
    if (level == "info") return wowee::core::LogLevel::INFO;
    if (level == "warn" || level == "warning") return wowee::core::LogLevel::WARNING;
    if (level == "error") return wowee::core::kLogLevelError;
    if (level == "fatal") return wowee::core::LogLevel::FATAL;
    return wowee::core::LogLevel::WARNING;
}

/// Point the client at the per-user data directory, if an extraction is there.
///
/// Only when nobody has said otherwise, and only when that directory actually
/// holds an extraction - so a first run with nothing installed still falls
/// through to Data/ beside the executable, which is what a development tree
/// and a portable install both want.
///
/// This was macOS only, which left the asset manager and the client with no
/// agreed destination on Linux or Windows at all: the manager wrote wherever
/// the terminal was and the client looked beside itself.
static void selectUserDataPath() {
    if (std::getenv("WOW_DATA_PATH")) return;

    const std::filesystem::path dataRoot = wowee::core::userDataRoot();
    if (!wowee::core::holdsExtraction(dataRoot)) return;

    wowee::core::setEnvVar("WOW_DATA_PATH", dataRoot.string().c_str(), /*overwrite=*/false);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
#ifdef __ANDROID__
    // Everything after this opens its files relative to the working directory,
    // which on Android is not a directory that holds any of them.
    wowee::core::enterResourceRoot();
#endif
#ifndef _WIN32
    // Writing to a socket the server has already closed raises SIGPIPE, whose
    // default action is to terminate - the client would vanish mid-frame with
    // no log line and no crash report, because SIGPIPE is not one of the
    // signals handled below. Ignoring it makes send() answer EPIPE instead,
    // which the send paths already handle: they log the failure and stop
    // writing, and the recv side sees the closed connection and disconnects.
    //
    // Done process-wide rather than per-socket: the flag that suppresses this
    // at the call site is spelled differently on each platform (MSG_NOSIGNAL
    // on Linux, the SO_NOSIGPIPE socket option on macOS/BSD), and nothing here
    // wants SIGPIPE for anything.
    std::signal(SIGPIPE, SIG_IGN);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    g_emergencyDisplay = XOpenDisplay(nullptr);
#endif
#ifdef WOWEE_HAS_BACKTRACE
    // SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGFPE, with the faulting address and
    // pc; see crashHandlerSigaction.
    installCrashHandler();
    std::signal(SIGTERM, [](int) { std::_Exit(1); });
    std::signal(SIGINT,  [](int) { std::_Exit(1); });
#elif defined(__ANDROID__)
    // Android already has a crash reporter, and it is a far better one than
    // this: debuggerd writes a symbolised tombstone and the abort message to
    // logcat. Ours writes a backtrace to stderr, which on Android goes nowhere,
    // and installing it costs the tombstone. So only the two exit signals here.
    std::signal(SIGTERM, crashHandler);
    std::signal(SIGINT,  crashHandler);
#else
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE,  crashHandler);
    std::signal(SIGTERM, crashHandler);
    std::signal(SIGINT,  crashHandler);
#endif
    // Change working directory so relative asset paths resolve from any launch
    // location. A signed macOS bundle keeps data in Contents/Resources because
    // Contents/MacOS may contain code only.
#ifdef __APPLE__
    {
        uint32_t bufSize = 0;
        _NSGetExecutablePath(nullptr, &bufSize);
        std::string exePath(bufSize, '\0');
        _NSGetExecutablePath(exePath.data(), &bufSize);
        const std::filesystem::path executableDir =
            std::filesystem::path(exePath.c_str()).parent_path();
        const std::filesystem::path resourceDir =
            executableDir.parent_path() / "Resources";
        const std::filesystem::path runtimeDir =
            std::filesystem::is_directory(resourceDir / "assets")
                ? resourceDir
                : executableDir;
        if (chdir(runtimeDir.c_str()) != 0) {}
    }
#elif defined(__linux__)
    {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = '\0'; if (chdir(dirname(buf)) != 0) {} }
    }
#endif

    // After the working directory is settled, and on every platform: this used
    // to sit inside the macOS branch above, so Linux and Windows went on
    // looking beside the executable for assets the asset manager had written
    // to their own per-user directory.
    selectUserDataPath();

    try {
        wowee::core::Logger::getInstance().setLogLevel(readLogLevelFromEnv());
        LOG_INFO("=== Wowee Native Client ===");
        // At warning level, with the platform, because a log sent in after a
        // crash is filtered to warnings and errors. Which build produced it is
        // the first question asked of any report and the log did not answer it:
        // a device loss on a version predating the fix for that very loss reads
        // exactly like a new one.
#if defined(_WIN32)
        constexpr const char* kPlatform = "windows";
#elif defined(__ANDROID__)
        constexpr const char* kPlatform = "android";
#elif defined(__APPLE__)
        constexpr const char* kPlatform = "macos";
#elif defined(__linux__)
        constexpr const char* kPlatform = "linux";
#else
        constexpr const char* kPlatform = "unknown-platform";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr const char* kArch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        constexpr const char* kArch = "x86-64";
#else
        constexpr const char* kArch = "unknown-arch";
#endif
        LOG_WARNING("Wowee ", wowee::core::kVersionString, " ", kPlatform, " ", kArch);
        LOG_INFO("Starting application...");

        // Seed portable config from the per-user location on first portable launch.
        wowee::core::migratePortableConfigIfNeeded();

        wowee::core::Application app;

        if (!app.initialize()) {
            LOG_FATAL("Failed to initialize application");
            return 1;
        }

        app.run();
        app.shutdown();

        LOG_INFO("Application exited successfully");
#if defined(__linux__) && !defined(__ANDROID__)
        if (g_emergencyDisplay) { XCloseDisplay(g_emergencyDisplay); g_emergencyDisplay = nullptr; }
#endif
        return 0;
    }
    catch (const std::exception& e) {
        releaseMouseGrab();
        LOG_FATAL("Unhandled exception: ", e.what());
        return 1;
    }
    catch (...) {
        releaseMouseGrab();
        LOG_FATAL("Unknown exception occurred");
        return 1;
    }
}
