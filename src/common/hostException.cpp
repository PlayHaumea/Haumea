#include "common/hostException.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#elif defined(__APPLE__)
#include <csignal>
#include <sys/ucontext.h>
#else
#include <csignal>
#include <initializer_list>
#include <ucontext.h> // IWYU pragma: keep
#include <unistd.h>
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

extern "C" bool StingerFixUnalignedFault(void* context);

namespace Common::HostException {

static std::atomic<GuestRegisterReader> g_guest_registers {nullptr};

static_assert(decltype(g_guest_registers)::is_always_lock_free);

void SetGuestRegisterReader(GuestRegisterReader reader) {
	g_guest_registers.store(reader, std::memory_order_release);
}

#if !defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) noexcept {
	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C ||
	    exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exception_record->ExceptionCode == 0x406D1388) {
		// Set a thread name.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		info.type = ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0]) {
			case 0: info.access_violation_type = AccessViolationType::Read; break;
			case 1: info.access_violation_type = AccessViolationType::Write; break;
			case 8: info.access_violation_type = AccessViolationType::Execute; break;
			default: info.access_violation_type = AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	} else if (exception_record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	info.rax = exception->ContextRecord->Rax;
	info.rbx = exception->ContextRecord->Rbx;
	info.rcx = exception->ContextRecord->Rcx;
	info.rdx = exception->ContextRecord->Rdx;
	info.rsi = exception->ContextRecord->Rsi;
	info.rdi = exception->ContextRecord->Rdi;
	info.rbp = exception->ContextRecord->Rbp;
	info.rsp = exception->ContextRecord->Rsp;
	info.r8  = exception->ContextRecord->R8;
	info.r9  = exception->ContextRecord->R9;
	info.r10 = exception->ContextRecord->R10;
	info.r11 = exception->ContextRecord->R11;
	info.r12 = exception->ContextRecord->R12;
	info.r13 = exception->ContextRecord->R13;
	info.r14 = exception->ContextRecord->R14;
	info.r15 = exception->ContextRecord->R15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

static struct sigaction g_previous_segv {};
static struct sigaction g_previous_bus {};
static struct sigaction g_previous_ill {};

static void ChainToPrevious(int sig, siginfo_t* si, void* uctx) {
	const struct sigaction& previous = sig == SIGBUS    ? g_previous_bus
	                                   : sig == SIGILL  ? g_previous_ill
	                                                    : g_previous_segv;
	if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
		previous.sa_sigaction(sig, si, uctx);
		return;
	}
	if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN &&
	    previous.sa_handler != nullptr) {
		previous.sa_handler(sig);
		return;
	}
	struct sigaction dfl {};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(sig, &dfl, nullptr);
}

#if defined(__x86_64__)

// Translate the x86-64 page-fault error code (mcontext __es.__err) into an access type.
// bit 1 (0x2) = write, bit 4 (0x10) = instruction fetch, otherwise a read.
static AccessViolationType DecodeAccess(uint64_t err) {
	if ((err & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((err & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

#else

// Read the access type out of the syndrome register (mcontext __es.__esr). EC (bits 26-31)
// 0x20/0x21 is an instruction abort; 0x24/0x25 is a data abort whose WnR bit separates
// write from read.
static AccessViolationType DecodeAccess(uint64_t esr) {
	const auto ec = (esr >> 26u) & 0x3fu;
	if (ec == 0x20u || ec == 0x21u) {
		return AccessViolationType::Execute;
	}
	if ((ec == 0x24u || ec == 0x25u) && ((esr >> 6u) & 0x1u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// The syndrome carries the fault CLASS in its low six bits, and a handler that only reads
// the direction bit cannot tell a permission fault from one it can never fix. HAUMEA_ESR=1.
static const bool g_report_syndrome = std::getenv("HAUMEA_ESR") != nullptr;
static std::atomic<uint64_t> g_syndrome_reports {0};

static void ReportSyndrome(int sig, const siginfo_t* si, uint64_t esr, uint64_t pc) {
	if (!g_report_syndrome || g_syndrome_reports.fetch_add(1, std::memory_order_relaxed) >= 4) {
		return;
	}
	std::fprintf(stderr,
	             "Haumea:Fault:Info: sig=%d code=%d at=%p pc=0x%016llx esr=0x%016llx ec=0x%02llx "
	             "dfsc=0x%02llx wnr=%llu\n",
	             sig, si != nullptr ? si->si_code : 0, si != nullptr ? si->si_addr : nullptr,
	             static_cast<unsigned long long>(pc), static_cast<unsigned long long>(esr),
	             static_cast<unsigned long long>((esr >> 26u) & 0x3fu),
	             static_cast<unsigned long long>(esr & 0x3fu),
	             static_cast<unsigned long long>((esr >> 6u) & 0x1u));
}

#endif

// POSIX signal handler that mirrors the Windows vectored handler: build an ExceptionInfo
// from the mcontext and dispatch. A resolved fault (handler returns true) simply returns,
// re-executing the faulting instruction against the now-fixed protection. An unresolved
// fault restores the default disposition so the retry terminates the process.
static void SignalHandler(int sig, siginfo_t* si, void* uctx) {
	auto*       uc = static_cast<ucontext_t*>(uctx);
	const auto* mc = uc->uc_mcontext;
	const auto& ss = mc->__ss;

	ExceptionInfo info {};
	info.native_code    = static_cast<uint32_t>(si->si_code);
	info.native_context = uctx;

	if (sig == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type = ExceptionType::AccessViolation;
#if defined(__x86_64__)
		info.access_violation_type = DecodeAccess(mc->__es.__err);
#else
		info.access_violation_type = DecodeAccess(mc->__es.__esr);
		ReportSyndrome(sig, si, mc->__es.__esr, ss.__pc);
		const auto esr = mc->__es.__esr;
		const auto fault_class = (esr >> 26u) & 0x3fu;
		if ((fault_class == 0x24u || fault_class == 0x25u) && (esr & 0x3fu) == 0x21u) {
			if (StingerFixUnalignedFault(uctx)) {
				return;
			}
			std::fprintf(stderr,
			             "Haumea:Fault:Error: unaligned access the CPU layer declined, at=%p "
			             "pc=0x%016llx esr=0x%016llx\n",
			             si != nullptr ? si->si_addr : nullptr,
			             static_cast<unsigned long long>(ss.__pc),
			             static_cast<unsigned long long>(esr));
			ChainToPrevious(sig, si, uctx);
			return;
		}
#endif
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(si->si_addr);
	}

#if defined(__x86_64__)
	info.exception_address = ss.__rip;
	info.rax               = ss.__rax;
	info.rbx               = ss.__rbx;
	info.rcx               = ss.__rcx;
	info.rdx               = ss.__rdx;
	info.rsi               = ss.__rsi;
	info.rdi               = ss.__rdi;
	info.rbp               = ss.__rbp;
	info.rsp               = ss.__rsp;
	info.r8                = ss.__r8;
	info.r9                = ss.__r9;
	info.r10               = ss.__r10;
	info.r11               = ss.__r11;
	info.r12               = ss.__r12;
	info.r13               = ss.__r13;
	info.r14               = ss.__r14;
	info.r15               = ss.__r15;
	info.guest_registers_valid = true;
#else
	(void)ss;
	const auto registers       = g_guest_registers.load(std::memory_order_acquire);
	info.guest_registers_valid = registers != nullptr && registers(uctx, info);
#endif

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return; // retry the faulting instruction against the fixed mapping
	}

	ChainToPrevious(sig, si, uctx);
}

#else

// x86-64 page-fault error bits.
constexpr uint64_t PAGE_FAULT_ERROR_WRITE       = 0x02;
constexpr uint64_t PAGE_FAULT_ERROR_INSTRUCTION = 0x10;

// Let the kernel handle an unresolved fault on retry.
static void ChainToDefault(int signal_number) noexcept {
	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	restore.sa_flags = 0;
	::sigaction(signal_number, &restore, nullptr);
}

static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	auto* context = static_cast<ucontext_t*>(native_context);
	auto* gregs   = context->uc_mcontext.gregs;

	ExceptionInfo info {};
	info.exception_address = static_cast<uint64_t>(gregs[REG_RIP]);
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type             = ExceptionType::AccessViolation;
		const auto error_code = static_cast<uint64_t>(gregs[REG_ERR]);
		if ((error_code & PAGE_FAULT_ERROR_INSTRUCTION) != 0) {
			info.access_violation_type = AccessViolationType::Execute;
		} else if ((error_code & PAGE_FAULT_ERROR_WRITE) != 0) {
			info.access_violation_type = AccessViolationType::Write;
		} else {
			info.access_violation_type = AccessViolationType::Read;
		}
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

	info.rax = static_cast<uint64_t>(gregs[REG_RAX]);
	info.rbx = static_cast<uint64_t>(gregs[REG_RBX]);
	info.rcx = static_cast<uint64_t>(gregs[REG_RCX]);
	info.rdx = static_cast<uint64_t>(gregs[REG_RDX]);
	info.rsi = static_cast<uint64_t>(gregs[REG_RSI]);
	info.rdi = static_cast<uint64_t>(gregs[REG_RDI]);
	info.rbp = static_cast<uint64_t>(gregs[REG_RBP]);
	info.rsp = static_cast<uint64_t>(gregs[REG_RSP]);
	info.r8  = static_cast<uint64_t>(gregs[REG_R8]);
	info.r9  = static_cast<uint64_t>(gregs[REG_R9]);
	info.r10 = static_cast<uint64_t>(gregs[REG_R10]);
	info.r11 = static_cast<uint64_t>(gregs[REG_R11]);
	info.r12 = static_cast<uint64_t>(gregs[REG_R12]);
	info.r13 = static_cast<uint64_t>(gregs[REG_R13]);
	info.r14 = static_cast<uint64_t>(gregs[REG_R14]);
	info.r15 = static_cast<uint64_t>(gregs[REG_R15]);

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	ChainToDefault(signal_number);
}

#endif

bool InstallHandler(Handler handler) {
	if (handler == nullptr) {
		return false;
	}

	uint32_t expected_state = 0;
	if (!g_install_state.compare_exchange_strong(expected_state, 1, std::memory_order_acq_rel)) {
		return expected_state == 2 && g_handler.load(std::memory_order_acquire) == handler;
	}

	g_handler.store(handler, std::memory_order_release);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (AddVectoredExceptionHandler(0, ExceptionFilter) == nullptr) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#elif defined(__APPLE__)
	struct sigaction sa {};
	sa.sa_sigaction = SignalHandler;
	sa.sa_flags     = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	// The guest signal-dispatch path (KernelRaiseException) interrupts threads with
	// SIGUSR1; block it while a fault is being resolved so a stop-the-world request
	// cannot preempt the handler between the protection fix and the retry.
	sigaddset(&sa.sa_mask, SIGUSR1);

	// macOS raises SIGBUS for protection faults on some paths and SIGSEGV on others;
	// SIGILL covers instructions the host cannot execute (routed to the x64 emulator).
	bool ok = sigaction(SIGSEGV, &sa, &g_previous_segv) == 0 &&
	          sigaction(SIGBUS, &sa, &g_previous_bus) == 0 &&
	          sigaction(SIGILL, &sa, &g_previous_ill) == 0;
	if (!ok) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("sigaction() failed to install the host fault handler\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	// Fault resolution needs the normal thread stack.
	action.sa_flags = SA_SIGINFO | SA_RESTART;

	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			g_handler.store(nullptr, std::memory_order_release);
			g_install_state.store(0, std::memory_order_release);
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif

	g_install_state.store(2, std::memory_order_release);
	return true;
}

} // namespace Common::HostException
