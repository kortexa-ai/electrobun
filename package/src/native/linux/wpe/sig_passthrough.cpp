// sig_passthrough.cpp — tiny helper library that saves/restores process
// signal handlers around the dlopen of libNativeWrapper_wpe.so.
//
// Why this exists: libwpewebkit-2.0 (transitively loaded as a dependency of
// libNativeWrapper_wpe.so) installs WTF::jscSignalHandler for SIGBUS/SIGSEGV/
// SIGFPE/SIGILL/SIGTRAP/SIGUSR2 at library-init time, overwriting Bun's
// handlers. Bun's runtime takes benign SIGBUS hits during normal operation
// (mmap'd file reads, etc.) which Bun's handler recovers from — but WTF's
// handler crashes (it expects JSC-VM thread-local state that Bun's threads
// don't have). The bun.report decoded stack frame is `WTF::jscSignalHandler`.
//
// This library does NOT link against any WebKit or WPE libs, so dlopening it
// does not trigger WTF::installSignalHandlers. The launcher dlopens this
// FIRST, calls electrobun_save_signals(), THEN dlopens libNativeWrapper_wpe
// (libwpewebkit's WTF clobbers signals), THEN calls electrobun_restore_signals()
// to put Bun's original handlers back. The WPEWebProcess child is forked
// and execs fresh — it builds its own signal handlers; this only affects
// the parent (Bun) process.

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

extern "C" {

// Diagnostic chain handler: logs si_addr / si_code, then chains to the
// previously installed (Bun's) handler so Bun still gets to crash-report.
static struct sigaction g_chainedActions[64];
static void diag_handler(int sig, siginfo_t* info, void* ucontext) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "[sig_passthrough] DIAG signo=%d code=%d si_addr=%p\n",
        sig, info ? info->si_code : -1,
        info ? info->si_addr : nullptr);
    if (n > 0) write(2, buf, n);
    if (sig < 0 || sig >= 64) { signal(sig, SIG_DFL); raise(sig); return; }
    struct sigaction& chained = g_chainedActions[sig];
    if (chained.sa_flags & SA_SIGINFO) {
        if (chained.sa_sigaction) chained.sa_sigaction(sig, info, ucontext);
    } else {
        if (chained.sa_handler == SIG_DFL) { signal(sig, SIG_DFL); raise(sig); return; }
        if (chained.sa_handler == SIG_IGN) return;
        if (chained.sa_handler) chained.sa_handler(sig);
    }
}

void electrobun_install_diag_handlers() {
    static const int sigs[] = { SIGBUS, SIGSEGV };
    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); ++i) {
        int s = sigs[i];
        struct sigaction old{};
        sigaction(s, nullptr, &old);
        g_chainedActions[s] = old;
        struct sigaction sa{};
        sa.sa_sigaction = diag_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(s, &sa, nullptr);
    }
    fprintf(stderr, "[sig_passthrough] installed diag handlers for SIGBUS/SIGSEGV\n"); fflush(stderr);
}

// We save handlers for the signals WTF clobbers. Sized to fit aarch64-glibc
// struct sigaction (~152 bytes); 6 signals × 256 bytes = ~1.5 KB total.
static const int kSavedSignals[] = { SIGBUS, SIGSEGV, SIGFPE, SIGILL, SIGTRAP, SIGUSR2 };
static constexpr int kNumSaved = sizeof(kSavedSignals) / sizeof(kSavedSignals[0]);
static struct sigaction g_savedActions[kNumSaved];
static int g_saveCount = 0;

void electrobun_save_signals() {
    g_saveCount = 0;
    for (int i = 0; i < kNumSaved; ++i) {
        if (sigaction(kSavedSignals[i], nullptr, &g_savedActions[i]) == 0) {
            ++g_saveCount;
        } else {
            memset(&g_savedActions[i], 0, sizeof(struct sigaction));
        }
    }
    fprintf(stderr, "[sig_passthrough] saved %d/%d signal handlers\n",
            g_saveCount, kNumSaved); fflush(stderr);
}

void electrobun_restore_signals() {
    int restored = 0;
    for (int i = 0; i < kNumSaved; ++i) {
        if (sigaction(kSavedSignals[i], &g_savedActions[i], nullptr) == 0) {
            ++restored;
        }
    }
    fprintf(stderr, "[sig_passthrough] restored %d/%d signal handlers\n",
            restored, kNumSaved); fflush(stderr);
}

}  // extern "C"
