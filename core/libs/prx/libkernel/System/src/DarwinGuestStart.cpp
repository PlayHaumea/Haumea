// Runtime half of the macOS (Mach-O) relinker output. Linux keeps guest code's %fs accesses on
// glibc's thread pointer, which already has the FreeBSD layout; Darwin owns %gs and leaves %fs
// unusable, so the relinker rewrites every %fs access into a stub that reads a per-image thread
// pointer from a pthread key and calls Aps5GuestTlsAllocate on a thread's first access.
#ifdef __APPLE__

#include "SceTypes.hpp"
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>

namespace {

// Written by the relinker into each image's __APS5DATA,__meta (MacOsImage.cpp, MetaTlsDescriptor).
struct TlsDescriptor {
    std::uint64_t version;
    const std::uint8_t* image;
    std::uint64_t imageSize;
    std::uint64_t blockSize;
    std::uint64_t alignment;
    std::uint64_t key;
};

// The thread control block sits at the thread pointer, after the TLS block (variant II). The
// relinker only lets guest code read offset 0 (the self pointer) and 0x28 (the stack guard).
constexpr std::size_t TcbBytes = 0x100;
constexpr std::size_t TcbStackGuard = 0x28;
constexpr std::size_t TcbDescriptor = 0x80;

std::mutex& RegisterMutex() {
    static std::mutex mutex;
    return mutex;
}

std::uint64_t StackGuard() {
    static const std::uint64_t guard = [] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32 | device()) & ~std::uint64_t{0xff};
    }();
    return guard;
}

void ReleaseBlock(void* threadPointer) {
    auto* tp = static_cast<std::uint8_t*>(threadPointer);
    const auto* descriptor = *reinterpret_cast<const TlsDescriptor* const*>(tp + TcbDescriptor);
    std::free(tp - descriptor->blockSize);
}

std::uint8_t* ThreadPointer(TlsDescriptor* descriptor) {
    if (descriptor->key == 0) {
        std::fprintf(stderr, "[aps5] guest TLS used before its image registered it\n");
        std::abort();
    }
    const auto key = static_cast<pthread_key_t>(descriptor->key);
    if (void* existing = pthread_getspecific(key)) return static_cast<std::uint8_t*>(existing);
    const std::size_t alignment = descriptor->alignment < 16 ? 16 : descriptor->alignment;
    void* block = nullptr;
    if (posix_memalign(&block, alignment, descriptor->blockSize + TcbBytes) != 0) {
        std::fprintf(stderr, "[aps5] cannot allocate %llu bytes of guest TLS\n", static_cast<unsigned long long>(descriptor->blockSize + TcbBytes));
        std::abort();
    }
    auto* base = static_cast<std::uint8_t*>(block);
    std::memset(base, 0, descriptor->blockSize + TcbBytes);
    if (descriptor->imageSize != 0) std::memcpy(base, descriptor->image, descriptor->imageSize);
    auto* tp = base + descriptor->blockSize;
    *reinterpret_cast<std::uint8_t**>(tp) = tp;
    *reinterpret_cast<std::uint64_t*>(tp + TcbStackGuard) = StackGuard();
    *reinterpret_cast<TlsDescriptor**>(tp + TcbDescriptor) = descriptor;
    pthread_setspecific(key, tp);
    return tp;
}

}

extern "C" {

void Aps5GuestTlsRegister_nid_no_patch(TlsDescriptor* descriptor) {
    std::lock_guard lock(RegisterMutex());
    if (descriptor->key != 0) return;
    pthread_key_t key = 0;
    if (const int error = pthread_key_create(&key, ReleaseBlock); error != 0) {
        std::fprintf(stderr, "[aps5] pthread_key_create for guest TLS failed: %s\n", std::strerror(error));
        std::abort();
    }
    descriptor->key = key;
}

__attribute__((visibility("hidden"), used)) std::uint8_t* Aps5GuestTlsAllocateSlow(TlsDescriptor* descriptor) {
    return ThreadPointer(descriptor);
}

// __tls_get_addr for the guest: the module word of the tls_index is the image's descriptor.
void* APS5_VABI Aps5GuestTlsGetAddr_nid_no_patch(const std::uint64_t* index) {
    auto* descriptor = reinterpret_cast<TlsDescriptor*>(index[0]);
    return ThreadPointer(descriptor) - descriptor->blockSize + index[1];
}

// Cocoa only works on the process's main thread, and SDL's video on macOS is Cocoa. The guest's
// entry therefore runs on a thread of its own and the main thread serves jobs that must run on it,
// such as the video output's window and present loop (VideoOutDriver.cpp).
namespace {

struct MainThreadJob {
    void (*function)(void*);
    void* context;
};

struct MainThreadQueue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<MainThreadJob> jobs;
    bool serving = false;
};

MainThreadQueue& MainQueue() {
    static auto* queue = new MainThreadQueue;
    return *queue;
}

struct GuestStart {
    void* entry;
    void* stack;
};

constexpr std::size_t GuestMainStackBytes = std::size_t{64} << 20;

void* RunGuestEntry(void* argument) {
    const auto start = *static_cast<GuestStart*>(argument);
    reinterpret_cast<void (APS5_VABI*)(void*, void*)>(start.entry)(start.stack, nullptr);
    std::fprintf(stderr, "[aps5] guest entry returned\n");
    std::exit(0);
}

}

extern "C" bool Aps5MainThreadServing_nid_no_patch() {
    auto& queue = MainQueue();
    std::lock_guard lock(queue.mutex);
    return queue.serving;
}

// Runs function(context) on the main thread without waiting for it. A long job (the present loop)
// holds the main thread until it returns.
extern "C" void Aps5MainThreadPost_nid_no_patch(void (*function)(void*), void* context) {
    auto& queue = MainQueue();
    {
        std::lock_guard lock(queue.mutex);
        queue.jobs.push_back({function, context});
    }
    queue.changed.notify_all();
}

// Called by the entry stub with the guest entry point and a SysV initial stack (argc, argv, envp).
[[noreturn]] void Aps5StartGuest_nid_no_patch(void* entry, void* stack) {
    static GuestStart start;
    start = {entry, stack};
    {
        auto& queue = MainQueue();
        std::lock_guard lock(queue.mutex);
        queue.serving = true;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GuestMainStackBytes);
    pthread_t guest;
    if (const int error = pthread_create(&guest, &attributes, RunGuestEntry, &start); error != 0) {
        std::fprintf(stderr, "[aps5] cannot start the guest thread: %s\n", std::strerror(error));
        std::exit(1);
    }
    pthread_attr_destroy(&attributes);
    auto& queue = MainQueue();
    for (;;) {
        MainThreadJob job{};
        {
            std::unique_lock lock(queue.mutex);
            queue.changed.wait(lock, [&] { return !queue.jobs.empty(); });
            job = queue.jobs.front();
            queue.jobs.pop_front();
        }
        job.function(job.context);
    }
}

}

// The rewritten %fs access calls this in the middle of guest code: the descriptor is in rax, the
// thread pointer goes back in rax, the stack has any alignment, and every other register must
// survive. The stub itself saves the flags.
__asm__(R"(
    .text
    .globl _Aps5GuestTlsAllocate_nid_no_patch
    .p2align 4
_Aps5GuestTlsAllocate_nid_no_patch:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $0x240, %rsp
    andq $-32, %rsp
    vmovdqu %ymm0, 0x000(%rsp)
    vmovdqu %ymm1, 0x020(%rsp)
    vmovdqu %ymm2, 0x040(%rsp)
    vmovdqu %ymm3, 0x060(%rsp)
    vmovdqu %ymm4, 0x080(%rsp)
    vmovdqu %ymm5, 0x0a0(%rsp)
    vmovdqu %ymm6, 0x0c0(%rsp)
    vmovdqu %ymm7, 0x0e0(%rsp)
    vmovdqu %ymm8, 0x100(%rsp)
    vmovdqu %ymm9, 0x120(%rsp)
    vmovdqu %ymm10, 0x140(%rsp)
    vmovdqu %ymm11, 0x160(%rsp)
    vmovdqu %ymm12, 0x180(%rsp)
    vmovdqu %ymm13, 0x1a0(%rsp)
    vmovdqu %ymm14, 0x1c0(%rsp)
    vmovdqu %ymm15, 0x1e0(%rsp)
    stmxcsr 0x200(%rsp)
    fnstcw 0x204(%rsp)
    movq %rax, %rdi
    callq _Aps5GuestTlsAllocateSlow
    fldcw 0x204(%rsp)
    ldmxcsr 0x200(%rsp)
    vmovdqu 0x000(%rsp), %ymm0
    vmovdqu 0x020(%rsp), %ymm1
    vmovdqu 0x040(%rsp), %ymm2
    vmovdqu 0x060(%rsp), %ymm3
    vmovdqu 0x080(%rsp), %ymm4
    vmovdqu 0x0a0(%rsp), %ymm5
    vmovdqu 0x0c0(%rsp), %ymm6
    vmovdqu 0x0e0(%rsp), %ymm7
    vmovdqu 0x100(%rsp), %ymm8
    vmovdqu 0x120(%rsp), %ymm9
    vmovdqu 0x140(%rsp), %ymm10
    vmovdqu 0x160(%rsp), %ymm11
    vmovdqu 0x180(%rsp), %ymm12
    vmovdqu 0x1a0(%rsp), %ymm13
    vmovdqu 0x1c0(%rsp), %ymm14
    vmovdqu 0x1e0(%rsp), %ymm15
    leaq -0x40(%rbp), %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbp
    retq
)");

#endif
