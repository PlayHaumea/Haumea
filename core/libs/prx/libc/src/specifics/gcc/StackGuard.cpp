#include <cstdint>
#include <cstdlib>

#ifdef __APPLE__
#include "prx/libc/include/general/ExportMacros.hpp"

// Darwin host code reads libSystem's ___stack_chk_guard. A libc.prx symbol of that name would
// capture every library's binds and then vanish when the NID patcher renames it, so the guest's
// guard has its own host name and is exported only by NID.
extern "C" std::uintptr_t Aps5GuestStackChkGuard = 0xDEADBEEFCAFEBABEull;
APS5_EXPORT("f7uOxY9mM1U", Aps5GuestStackChkGuard);
#else
std::uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABEull;
#endif
