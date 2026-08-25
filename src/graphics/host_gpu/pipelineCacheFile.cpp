#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Libs::Graphics {

namespace {

std::string& CacheFolderOverride() {
	static std::string folder;
	return folder;
}

std::filesystem::path CacheFolder() {
	const auto& override_folder = CacheFolderOverride();
	if (!override_folder.empty()) {
		return std::filesystem::path(override_folder);
	}
	return std::filesystem::path("_Cache");
}

std::filesystem::path PipelineCachePath() {
	return CacheFolder() / "pipeline.bin";
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
	std::vector<uint8_t> bytes;
	std::error_code      ec;
	const auto           size = std::filesystem::file_size(path, ec);
	if (ec || size == 0) {
		return bytes;
	}
	FILE* file = std::fopen(path.string().c_str(), "rb");
	if (file == nullptr) {
		return bytes;
	}
	bytes.resize(static_cast<size_t>(size));
	if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		bytes.clear();
	}
	std::fclose(file);
	return bytes;
}

bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	FILE* file = std::fopen(path.string().c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	std::fclose(file);
	return ok;
}

} // namespace

void GraphicContext::CreatePipelineCache() {
	EXIT_IF(device == nullptr);
	if (pipeline_cache != nullptr) {
		return;
	}

	const auto            path  = PipelineCachePath();
	std::vector<uint8_t>  blob  = ReadFile(path);
	vk::PipelineCacheCreateInfo info {};
	info.initialDataSize = blob.size();
	info.pInitialData    = blob.empty() ? nullptr : blob.data();

	auto result = device.createPipelineCache(&info, nullptr, &pipeline_cache);
	if (result != vk::Result::eSuccess) {
		pipeline_cache = nullptr;
		info.initialDataSize = 0;
		info.pInitialData    = nullptr;
		result               = device.createPipelineCache(&info, nullptr, &pipeline_cache);
		if (result != vk::Result::eSuccess) {
			pipeline_cache = nullptr;
			LOGF("PipelineCache: create failed: %s\n", VulkanToString(result).c_str());
			return;
		}
	}
	LOGF("PipelineCache: loaded %" PRIu64 " bytes from %s\n", static_cast<uint64_t>(blob.size()),
	     path.string().c_str());
}

void SetPipelineCacheFolder(const char* folder) {
	CacheFolderOverride() = (folder != nullptr ? folder : "");
}

void GraphicContext::SavePipelineCache() const {
	if (device == nullptr || pipeline_cache == nullptr) {
		return;
	}
	size_t size   = 0;
	auto   result = device.getPipelineCacheData(pipeline_cache, &size, nullptr);
	if (result != vk::Result::eSuccess || size == 0) {
		return;
	}
	std::vector<uint8_t> blob(size);
	result = device.getPipelineCacheData(pipeline_cache, &size, blob.data());
	if (result != vk::Result::eSuccess) {
		return;
	}
	blob.resize(size);
	const auto path = PipelineCachePath();
	if (!WriteFile(path, blob)) {
		LOGF("PipelineCache: could not write %s\n", path.string().c_str());
		return;
	}
	LOGF("PipelineCache: wrote %" PRIu64 " bytes to %s\n", static_cast<uint64_t>(blob.size()),
	     path.string().c_str());
}

void GraphicContext::DestroyPipelineCache() {
	if (device == nullptr || pipeline_cache == nullptr) {
		return;
	}
	device.destroyPipelineCache(pipeline_cache, nullptr);
	pipeline_cache = nullptr;
}

} // namespace Libs::Graphics
