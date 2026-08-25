#ifndef KYTY_COMMON_GUEST_CPU_H_
#define KYTY_COMMON_GUEST_CPU_H_

#include "common/common.h"
#include "common/hostException.h"

namespace Common::GuestCpu {

struct Registers {
	uint64_t rax     = 0;
	uint64_t rbx     = 0;
	uint64_t rcx     = 0;
	uint64_t rdx     = 0;
	uint64_t rsi     = 0;
	uint64_t rdi     = 0;
	uint64_t rbp     = 0;
	uint64_t rsp     = 0;
	uint64_t r8      = 0;
	uint64_t r9      = 0;
	uint64_t r10     = 0;
	uint64_t r11     = 0;
	uint64_t r12     = 0;
	uint64_t r13     = 0;
	uint64_t r14     = 0;
	uint64_t r15     = 0;
	uint64_t rip     = 0;
	uint64_t rflags  = 0;
	uint64_t fs_base = 0;
	uint8_t  xmm[16][16] = {};
};

struct CallFrame {
	uint64_t return_address = 0;
	uint64_t stack_pointer  = 0;
};

struct Interface {
	void (*CodeMapped)(uint64_t vaddr, uint64_t size)   = nullptr;
	void (*CodeModified)(uint64_t vaddr, uint64_t size) = nullptr;
	void (*CodeUnmapped)(uint64_t vaddr, uint64_t size) = nullptr;

	uint64_t (*Call)(uint64_t entry, const uint64_t* args, uint32_t arg_count) = nullptr;

	uint64_t (*CallOnStack)(uint64_t entry, const uint64_t* args, uint32_t arg_count,
	                        uint64_t stack_top) = nullptr;

	uint64_t (*HostThunk)(uint64_t host_function) = nullptr;

	bool (*ReadRegisters)(Registers* registers)        = nullptr;
	bool (*WriteRegisters)(const Registers& registers) = nullptr;

	bool (*CurrentCallFrame)(CallFrame* frame) = nullptr;

	bool (*DeliverSignal)(uint64_t thread_id, int signal_number, void* guest_ucontext) = nullptr;
};

void Install(const Interface& interface);

[[nodiscard]] bool IsInstalled();

[[nodiscard]] const Interface& Get();

} // namespace Common::GuestCpu

#endif /* KYTY_COMMON_GUEST_CPU_H_ */
