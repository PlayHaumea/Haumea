#include "common/guestCpu.h"

namespace Common::GuestCpu {

static Interface g_interface {};
static bool      g_installed = false;

void Install(const Interface& interface) {
	g_interface = interface;
	g_installed = true;
}

bool IsInstalled() {
	return g_installed;
}

const Interface& Get() {
	return g_interface;
}

} // namespace Common::GuestCpu
