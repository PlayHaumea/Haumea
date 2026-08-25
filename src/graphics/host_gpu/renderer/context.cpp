#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
namespace Libs::Graphics {

namespace {

enum class SubmitTraceKind : uint32_t { Record, Phase, Submit };

struct SubmitTraceEntry {
	uint64_t        order      = 0;
	uint64_t        submit_seq = 0;
	uint64_t        submit_id  = 0;
	uint64_t        arg4       = 0;
	uint32_t        slot       = 0;
	uint32_t        op         = 0;
	uint32_t        phase      = 0;
	uint32_t        arg0       = 0;
	uint32_t        arg1       = 0;
	uint32_t        arg2       = 0;
	uint32_t        arg3       = 0;
	SubmitTraceKind kind       = SubmitTraceKind::Record;
};

constexpr uint64_t SUBMIT_TRACE_SIZE      = 256;
constexpr uint64_t SUBMIT_SYNC_TIMEOUT_NS = 5ull * 1000ull * 1000ull * 1000ull;

bool SubmitSyncEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("MAGNUS_SUBMIT_SYNC");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

std::array<SubmitTraceEntry, SUBMIT_TRACE_SIZE> g_submit_trace {};
std::atomic<uint64_t>                           g_submit_trace_next {0};

const char* DebugOpName(uint32_t op) {
	switch (static_cast<CommandBufferDebugOp>(op)) {
		case CommandBufferDebugOp::DispatchDirect: return "DispatchDirect";
		case CommandBufferDebugOp::DrawIndex: return "DrawIndex";
		case CommandBufferDebugOp::DrawIndexAuto: return "DrawIndexAuto";
		case CommandBufferDebugOp::EopWrite: return "EopWrite";
		case CommandBufferDebugOp::EopInterrupt: return "EopInterrupt";
		case CommandBufferDebugOp::EopWriteBack: return "EopWriteBack";
		case CommandBufferDebugOp::EopFlip: return "EopFlip";
		case CommandBufferDebugOp::EopWriteBackFlip: return "EopWriteBackFlip";
		case CommandBufferDebugOp::EopOnlyFlip: return "EopOnlyFlip";
		case CommandBufferDebugOp::DispatchIndirect: return "DispatchIndirect";
		case CommandBufferDebugOp::Unknown: return "Unknown";
	}
	return "?";
}

void DumpSubmitTrace() {
	const uint64_t total = g_submit_trace_next.load(std::memory_order_relaxed);
	const uint64_t first = total > SUBMIT_TRACE_SIZE ? total - SUBMIT_TRACE_SIZE : 0;
	std::printf("submit trace: recorded=%llu showing=%llu ring=%llu\n",
	            static_cast<unsigned long long>(total),
	            static_cast<unsigned long long>(total - first),
	            static_cast<unsigned long long>(SUBMIT_TRACE_SIZE));
	for (uint64_t index = first; index < total; index++) {
		const auto& entry = g_submit_trace[index % SUBMIT_TRACE_SIZE];
		if (entry.order != index + 1) {
			std::printf("  [%llu] overwritten while dumping\n",
			            static_cast<unsigned long long>(index + 1));
			continue;
		}
		switch (entry.kind) {
			case SubmitTraceKind::Submit:
				std::printf("  [%llu] SUBMIT slot=%u seq=%llu last_op=%s last_submit_id=%llu\n",
				            static_cast<unsigned long long>(entry.order), entry.slot,
				            static_cast<unsigned long long>(entry.submit_seq),
				            DebugOpName(entry.op),
				            static_cast<unsigned long long>(entry.submit_id));
				break;
			case SubmitTraceKind::Phase:
				std::printf("  [%llu] phase  slot=%u op=%s submit_id=%llu phase=0x%x "
				            "index_count=%u flags=%u instance_count=%u first_instance=0x%llx\n",
				            static_cast<unsigned long long>(entry.order), entry.slot,
				            DebugOpName(entry.op),
				            static_cast<unsigned long long>(entry.submit_id), entry.phase,
				            entry.arg0, entry.arg1, entry.arg2,
				            static_cast<unsigned long long>(entry.arg4));
				break;
			case SubmitTraceKind::Record:
				std::printf("  [%llu] record slot=%u op=%s submit_id=%llu "
				            "args=%u,%u,%u,%u,0x%016llx\n",
				            static_cast<unsigned long long>(entry.order), entry.slot,
				            DebugOpName(entry.op),
				            static_cast<unsigned long long>(entry.submit_id), entry.arg0,
				            entry.arg1, entry.arg2, entry.arg3,
				            static_cast<unsigned long long>(entry.arg4));
				break;
		}
	}
	std::fflush(stdout);
}

void ReportVulkanFatal(const char* what, vk::Result result, uint32_t slot, uint64_t submit_seq,
                       uint32_t debug_op, uint64_t debug_submit, uint32_t arg0, uint32_t arg1,
                       uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), slot=%u submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, VulkanToString(result).c_str(), static_cast<int>(result), slot, submit_seq, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), slot=%u submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, VulkanToString(result).c_str(), static_cast<int>(result), slot, submit_seq,
	            debug_op, debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
	DumpSubmitTrace();
}

void RecordSubmitTrace(SubmitTraceEntry entry) {
	const uint64_t order = g_submit_trace_next.fetch_add(1, std::memory_order_relaxed);
	auto&          slot  = g_submit_trace[order % SUBMIT_TRACE_SIZE];
	entry.order          = 0;
	slot                 = entry;
	slot.order           = order + 1;
}

} // namespace

FenceResourceRetainer::~FenceResourceRetainer() {
	if (!m_resources.empty()) {
		EXIT("fence resource retainer destroyed before release\n");
	}
}

void FenceResourceRetainer::Retain(std::shared_ptr<void> resource) {
	if (resource == nullptr) {
		EXIT("cannot retain a null fence resource\n");
	}
	if (std::ranges::none_of(m_resources, [&resource](const auto& retained) {
		    return retained.get() == resource.get();
	    })) {
		m_resources.push_back(std::move(resource));
	}
}

void FenceResourceRetainer::ReleaseAfterFence() noexcept {
	m_resources.clear();
}

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_scheduler(scheduler), m_graphics(scheduler.Graphics()),
      m_slot(scheduler.AllocateCommandBuffer()) {}

CommandBuffer::~CommandBuffer() {
	Release();
}

bool CommandBuffer::IsInvalid() const {
	return m_slot == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());

	const auto handle = m_slot->buffer;
	EXIT_IF(handle == nullptr);
	return handle;
}

void CommandBuffer::Release() {
	EXIT_IF(IsInvalid());

	Common::LockGuard lock(*m_slot->pool_mutex);

	WaitForFence();

	m_slot->busy = false;
	m_slot->Reset();
	ReleaseResourcesAfterFence();
	m_slot = nullptr;

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::RetireBufferAfterFence(std::unique_ptr<VulkanBuffer> buffer) {
	if (IsInvalid() || m_execute || buffer == nullptr || buffer->buffer == nullptr) {
		EXIT("cannot retire a buffer on an invalid or submitted command buffer\n");
	}
	m_retired_buffers.push_back(std::move(buffer));
}

void CommandBuffer::RetainResourceUntilFence(std::shared_ptr<void> resource) {
	if (IsInvalid() || m_execute) {
		EXIT("cannot retain a resource on an invalid or submitted command buffer\n");
	}
	m_fence_resources.Retain(std::move(resource));
}

void CommandBuffer::RecycleDescriptorAfterFence(VulkanDescriptorSet& set) {
	m_descriptor_sets_after_fence.push_back(&set);
}

void CommandBuffer::RecycleDescriptorsAfterFence() {
	for (auto* set: m_descriptor_sets_after_fence) {
		m_context.GetDescriptorCache().Recycle(*set);
	}
	m_descriptor_sets_after_fence.clear();
}

void CommandBuffer::Begin() const {
	EXIT_IF(m_rendering);
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.sType            = vk::StructureType::eCommandBufferBeginInfo;
	begin_info.pNext            = nullptr;
	begin_info.flags            = {};
	begin_info.pInheritanceInfo = nullptr;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
	RecordSubmitTrace({.submit_id = submit_id,
	                   .arg4      = arg4,
	                   .slot      = m_slot != nullptr ? m_slot->id : UINT32_MAX,
	                   .op        = op,
	                   .arg0      = arg0,
	                   .arg1      = arg1,
	                   .arg2      = arg2,
	                   .arg3      = arg3,
	                   .kind      = SubmitTraceKind::Record});
}

void CommandBuffer::SetDrawPhase(uint32_t op, uint64_t submit_id, uint32_t phase,
                                 uint32_t index_count, uint32_t flags, uint32_t instance_count,
                                 uint64_t first_instance) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = phase;
	m_debug_arg1      = index_count;
	m_debug_arg2      = flags;
	m_debug_arg3      = instance_count;
	m_debug_arg4      = first_instance;
	RecordSubmitTrace({.submit_id = submit_id,
	                   .arg4      = first_instance,
	                   .slot      = m_slot != nullptr ? m_slot->id : UINT32_MAX,
	                   .op        = op,
	                   .phase     = phase,
	                   .arg0      = index_count,
	                   .arg1      = flags,
	                   .arg2      = instance_count,
	                   .kind      = SubmitTraceKind::Phase});
}

void CommandBuffer::Execute(const SubmitInfo& submit) {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores > SubmitInfo::MaxSemaphores);

	auto buffer = Handle();
	auto fence  = m_slot->fence;

	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.sType                     = vk::StructureType::eTimelineSemaphoreSubmitInfo;
	timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
	timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
	timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
	timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

	vk::SubmitInfo submit_info {};
	submit_info.sType                = vk::StructureType::eSubmitInfo;
	submit_info.pNext                = &timeline_info;
	submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
	submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
	submit_info.pWaitDstStageMask    = submit.wait_stages.data();
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
	submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

	auto& graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	auto result = graphics.device.resetFences(1, &fence);
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkResetFences (before submit)", result, m_slot->id, m_submit_seq,
		                  m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2,
		                  m_debug_arg3, m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: slot=%u waits=%u signals=%u debug_op=%u debug_submit=%" PRIu64
		     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_slot->id, submit.num_wait_semaphores, submit.num_signal_semaphores, m_debug_op,
		     m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}

	{
		Common::LockGuard lock(graphics.queue_mutex);
		m_submit_seq = m_scheduler.NextSubmitSequence();
		result       = graphics.queue.submit(1, &submit_info, fence);
	}

	m_execute      = true;
	m_fence_waited = false;
	RecordSubmitTrace({.submit_seq = m_submit_seq,
	                   .submit_id  = m_debug_submit_id,
	                   .slot       = m_slot->id,
	                   .op         = m_debug_op,
	                   .kind       = SubmitTraceKind::Submit});

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, m_slot->id, m_submit_seq, m_debug_op,
		                  m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		                  m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (SubmitSyncEnabled()) {
		static std::atomic_bool reported {false};
		const auto sync = graphics.device.waitForFences(1, &fence, VK_TRUE, SUBMIT_SYNC_TIMEOUT_NS);
		if (sync != vk::Result::eSuccess && !reported.exchange(true, std::memory_order_relaxed)) {
			ReportVulkanFatal("vkWaitForFences (submit sync)", sync, m_slot->id, m_submit_seq,
			                  m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1,
			                  m_debug_arg2, m_debug_arg3, m_debug_arg4);
		}
	}
}

void CommandBuffer::WaitForFence() {
	FinalizeFence(false);
}

void CommandBuffer::WaitForFenceOnly() {
	EXIT_IF(IsInvalid());
	if (!m_execute || m_fence_waited) {
		return;
	}
	auto device = m_graphics.device;
	auto result = device.waitForFences(1, &m_slot->fence, VK_TRUE, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkWaitForFences", result, m_slot->id, m_submit_seq, m_debug_op,
		                  m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		                  m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	m_fence_waited = true;
}

void CommandBuffer::WaitForFenceAndReset() {
	FinalizeFence(true);
}

void CommandBuffer::FinalizeFence(bool reset_recording) {
	const bool was_executed = m_execute;
	WaitForFenceOnly();
	if (was_executed) {
		m_execute      = false;
		m_fence_waited = false;
		if (reset_recording) {
			Common::LockGuard lock(*m_slot->pool_mutex);
			m_slot->Reset();
		}
	}
	if (was_executed) {
		ReleaseResourcesAfterFence();
	}
	DeleteBuffersAfterFence();
}

void CommandBuffer::ReleaseResourcesAfterFence() {
	RecycleDescriptorsAfterFence();
	m_fence_resources.ReleaseAfterFence();
}

void CommandBuffer::DeleteBuffersAfterFence() {
	for (const auto& buffer: m_retired_buffers) {
		m_graphics.DeleteBuffer(*buffer);
	}
	m_retired_buffers.clear();
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	if (m_rendering && m_render_state == state) {
		return;
	}
	EndRendering();

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].sType        = vk::StructureType::eRenderingAttachmentInfo;
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.sType       = vk::StructureType::eRenderingAttachmentInfo;
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.sType       = vk::StructureType::eRenderingAttachmentInfo;
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.sType                = vk::StructureType::eRenderingInfo;
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics
