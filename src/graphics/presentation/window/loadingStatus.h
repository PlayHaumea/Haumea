#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOADINGSTATUS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOADINGSTATUS_H_

#include "SDL_video.h"

#include <cstdint>
#include <string_view>

namespace Libs::Graphics {

void LoadingStatusUpdate(SDL_Window* window, std::string_view title, std::string_view title_id,
	                     std::string_view artwork_path);
void LoadingStatusFinish(SDL_Window* window, std::string_view title_id);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOADINGSTATUS_H_
