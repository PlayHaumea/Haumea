// Desktop launcher for the macOS x86-64 build (Rosetta): runs one title's app0 directory.
//   haumea-mac <app0 dir>
// HAUMEA_VERBOSE=1 sends the emulator's output to the console.

#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <app0 dir>\n", argv[0]);
		return 2;
	}
	Common::VirtualMemory::Init();
	Common::InitializeThreads();

	Emulator::RunOptions options;
	options.config.printf_direction = std::getenv("HAUMEA_VERBOSE") != nullptr
	                                      ? Config::OutputDirection::Console
	                                      : Config::OutputDirection::Silent;
	options.app0_dir = std::filesystem::absolute(argv[1]);
	options.elf      = "/app0/eboot.bin";
	Emulator::Run(options);
	return 0;
}
