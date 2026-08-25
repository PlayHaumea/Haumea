#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_METALFX_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_METALFX_H_

#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics::MetalFx {

struct Frames {
	VulkanImage* real = nullptr;
	VulkanImage* mid  = nullptr;
};

void   SetSettings(bool spatial, bool temporal, bool frame_interpolation);
bool   Wanted();
Frames Run(GraphicContext& graphics, CommandBuffer& command, VulkanImage& frame,
           vk::Extent2D output, bool allow_interpolation, double target_fps);
void   Reset();

}

#endif
