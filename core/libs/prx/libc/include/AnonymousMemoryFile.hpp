#pragma once

#ifndef _WIN32
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace AnonymousMemoryFile {

// A descriptor for anonymous shared memory: Linux's memfd_create; on macOS, which has none, a POSIX shared memory
// object unlinked as soon as it is open. macOS sets such an object's size only once (a second ftruncate fails).
inline int Create([[maybe_unused]] const char* name) {
#if defined(__APPLE__)
    static std::atomic<unsigned> counter{0};
    for (int attempt = 0; attempt < 64; ++attempt) {
        char path[31];  // macOS limits shared memory names to 31 bytes
        std::snprintf(path, sizeof(path), "/aps5.%d.%u", static_cast<int>(::getpid()), counter.fetch_add(1));
        const int descriptor = ::shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0) {
            ::shm_unlink(path);
            ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
            return descriptor;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
#else
    return ::memfd_create(name, MFD_CLOEXEC);
#endif
}

}  // namespace AnonymousMemoryFile
#endif
