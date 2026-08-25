#ifndef KYTY_COMMON_ASSERT_H_
#define KYTY_COMMON_ASSERT_H_

#include "common/common.h"
#include "common/logging/log.h"

#include <cstdlib>
#include <string_view>

namespace Common {

int  DbgExitHandler(char const* file, int line, std::string_view text);
int  DbgExitHandler(char const* file, int line, fmt::text_style style, std::string_view text);
int  DbgExitIfHandler(char const* expr, char const* file, int line);
int  DbgNotImplementedHandler(char const* expr, char const* file, int line);
[[noreturn]] void DbgExit(int status);

} // namespace Common

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#define EXIT_HALT() Common::DbgExit(321)
#else
#define EXIT_HALT() std::_Exit(321)
#endif

#ifndef KYTY_FINAL
#define EXIT_IF(x)                                                                                 \
	do {                                                                                           \
		if (x) {                                                                                   \
			Common::DbgExitIfHandler(#x, __FILE__, __LINE__);                                      \
			EXIT_HALT();                                                                            \
		}                                                                                          \
	} while (0)
#else
#define EXIT_IF(x)                                                                                 \
	do {                                                                                           \
		constexpr bool kyty_exit_if_disabled = false && (x);                                       \
		(void)kyty_exit_if_disabled;                                                               \
	} while (0)
#endif

#define EXIT(...)                                                                                  \
	do {                                                                                           \
		Common::DbgExitHandler(__FILE__, __LINE__, ::fmt::sprintf(__VA_ARGS__));                   \
		EXIT_HALT();                                                                                \
	} while (0)

#define EXIT_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		Common::DbgExitHandler(__FILE__, __LINE__, (style), ::fmt::sprintf(__VA_ARGS__));          \
		EXIT_HALT();                                                                                \
	} while (0)

#define EXIT_NOT_IMPLEMENTED(x)                                                                    \
	do {                                                                                           \
		if (x) {                                                                                   \
			Common::DbgNotImplementedHandler(#x, __FILE__, __LINE__);                              \
			EXIT_HALT();                                                                            \
		}                                                                                          \
	} while (0)
#define KYTY_NOT_IMPLEMENTED EXIT_NOT_IMPLEMENTED(true)

#endif /* KYTY_COMMON_ASSERT_H_ */
