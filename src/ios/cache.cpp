#include <cstddef>
#include <libkern/OSCacheControl.h>

extern "C" void __clear_cache(void* begin, void* end) {
	auto* first = static_cast<std::byte*>(begin);
	auto* last  = static_cast<std::byte*>(end);
	sys_icache_invalidate(begin, static_cast<size_t>(last - first));
}
