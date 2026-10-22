// wpe_helper — standalone process that loads libNativeWrapper_wpe.so and
// drives a single fullscreen WPE webview at the URL given on argv[1].
//
// This is the same pattern as the §13 standalone harness (which renders
// successfully): main thread runs startEventLoop, a worker pthread calls
// createGTKWindow + initWebview after a short delay. The reason it lives
// in its own process: when libwpewebkit-2.0 is loaded into the same
// process as Bun, the WPEWebProcess fork (clone3 with CLONE_VM|CLONE_VFORK)
// somehow corrupts a function pointer in Bun's data; Bun then takes
// SIGBUS (BUS_ADRALN — branch to an unaligned address) and crashes inside
// its own signal handler. We could not fix this from the parent side. By
// running WebKit in its own process we sidestep the issue entirely.

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

typedef int (*startEventLoop_t)(const char*, const char*, const char*);
typedef void* (*createGTKWindow_t)(uint32_t, double, double, double, double, const char*,
    void*, void*, void*, void*, void*, void*, const char*, int);
typedef void* (*initWebview_t)(uint32_t, void*, const char*, const char*,
    double, double, double, double, int, const char*,
    void*, void*, void*, void*, void*, const char*, const char*, const char*, int, int);

struct WorkerArgs {
    void* lib;
    const char* url;
};

static void* worker_thread(void* a) {
    auto* args = static_cast<WorkerArgs*>(a);
    sleep(1);
    fprintf(stderr, "[wpe_helper] worker thread starting\n"); fflush(stderr);

    auto createGTKWindow = (createGTKWindow_t)dlsym(args->lib, "createGTKWindow");
    auto initWebview     = (initWebview_t)dlsym(args->lib, "initWebview");
    if (!createGTKWindow || !initWebview) {
        fprintf(stderr, "[wpe_helper] dlsym failed: createGTKWindow=%p initWebview=%p\n",
                createGTKWindow, initWebview); fflush(stderr);
        return nullptr;
    }

    void* window = createGTKWindow(1, 0, 0, 1920, 480, "HelloElectrobun",
                                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                   "default", 0);
    fprintf(stderr, "[wpe_helper] createGTKWindow returned %p\n", window); fflush(stderr);

    void* view = initWebview(1, window, "wpe", args->url,
                             0, 0, 1920, 480, 1, "",
                             nullptr, nullptr, nullptr, nullptr, nullptr,
                             "", "", "", 0, 0);
    fprintf(stderr, "[wpe_helper] initWebview returned %p (url=%s)\n",
            view, args->url); fflush(stderr);
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: wpe_helper <url> [identifier] [name] [channel]\n");
        return 1;
    }
    const char* url = argv[1];
    const char* identifier = (argc > 2) ? argv[2] : "ai.kortexa.hello-embedded";
    const char* name = (argc > 3) ? argv[3] : "HelloElectrobun";
    const char* channel = (argc > 4) ? argv[4] : "dev";

    char selfPath[4096];
    ssize_t n = readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);
    if (n < 0) { perror("readlink"); return 1; }
    selfPath[n] = 0;
    char* slash = strrchr(selfPath, '/');
    if (slash) *slash = 0;
    char libPath[4096];
    snprintf(libPath, sizeof(libPath), "%s/libNativeWrapper.so", selfPath);

    fprintf(stderr, "[wpe_helper] dlopen %s\n", libPath); fflush(stderr);
    void* lib = dlopen(libPath, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "[wpe_helper] dlopen failed: %s\n", dlerror()); return 1; }

    auto startEventLoop = (startEventLoop_t)dlsym(lib, "startEventLoop");
    if (!startEventLoop) { fprintf(stderr, "[wpe_helper] missing startEventLoop\n"); return 1; }

    if (!getenv("WPE_HELPER_SKIP_WORKER")) {
        pthread_t th;
        auto* wargs = new WorkerArgs{ lib, url };
        pthread_create(&th, nullptr, worker_thread, wargs);
    } else {
        fprintf(stderr, "[wpe_helper] WPE_HELPER_SKIP_WORKER=1 — primeWpeView only\n"); fflush(stderr);
    }

    fprintf(stderr, "[wpe_helper] entering startEventLoop\n"); fflush(stderr);
    startEventLoop(identifier, name, channel);
    fprintf(stderr, "[wpe_helper] startEventLoop returned, exiting\n"); fflush(stderr);
    return 0;
}
