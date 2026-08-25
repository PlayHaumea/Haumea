#include "common/assert.h"

#include "common/debug.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

namespace Common {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_BUILD == KYTY_BUILD_DEBUG &&                    \
    KYTY_COMPILER == KYTY_COMPILER_CLANG
constexpr int PRINT_STACK_FROM = 4;
#else
constexpr int PRINT_STACK_FROM = 2;
#endif

static std::string BuildFatalReport(std::string_view text, const char* file, int line) {
	std::string_view source = file != nullptr ? std::string_view(file) : std::string_view();
	const auto       slash  = source.find_last_of("/\\");
	if (slash != std::string_view::npos) {
		source.remove_prefix(slash + 1);
	}

	std::string report = fmt::format("Magnus:Core:Fatal: {} file=\"{}\" line={} build=\"{}\"\n",
	                                 text, source, line, KYTY_BUILD_LABEL);
	if (std::getenv("MAGNUS_VERBOSE") != nullptr) {
		DebugStack stack;
		DebugStack::Trace(&stack);
		for (int i = PRINT_STACK_FROM; i < stack.depth; i++) {
			report += fmt::format("Magnus:Core:Trace: frame={} address=0x{:016x}\n",
			                      i - PRINT_STACK_FROM,
			                      static_cast<uint64_t>(stack.GetAddr(i)));
		}
	}
	return report;
}

static int DbgReport(std::string_view text, const char* file, int line) {
	Log::WriteFatal(BuildFatalReport(text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport(fmt::format("condition ({}) is true", expr), file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	return DbgReport(fmt::format("not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	Log::WriteFatal(BuildFatalReport(text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	Log::WriteFatal(style, BuildFatalReport(text, file, line));
	return 1;
}

[[noreturn]] void DbgExit(int status) {
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
