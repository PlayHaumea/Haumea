#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
void                     WindowRun();
void                     WindowShutdown();

void          SetExternalSurface(void* metal_layer, uint32_t width, uint32_t height);
[[nodiscard]] void* ExternalSurface();
void          ExternalSurfaceSize(uint32_t* width, uint32_t* height);
void          SetAppPaused(bool paused);
[[nodiscard]] bool IsAppPaused();

[[nodiscard]] uint64_t GuestFrameCount();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
