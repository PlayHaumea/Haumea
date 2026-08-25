#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERINFOCOLLECTION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ShaderInfoOptions {
	const ShaderVertexInputInfo*  vertex  = nullptr;
	const ShaderPixelInputInfo*   pixel   = nullptr;
	const ShaderComputeInputInfo* compute = nullptr;
};

bool CollectShaderInfo(Program& program, const ShaderInfoOptions& options, std::string* error);

}

#endif
