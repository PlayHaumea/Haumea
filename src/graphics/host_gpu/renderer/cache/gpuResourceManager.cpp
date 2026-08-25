#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/vm_map.h>

namespace Libs::Graphics {

namespace {

const bool g_report_faults = std::getenv("MAGNUS_GPUFAULT") != nullptr;

std::atomic<uint64_t> g_fault_reads {0};
std::atomic<uint64_t> g_fault_writes {0};
std::atomic<uint64_t> g_fault_repeats {0};
std::atomic<uint64_t> g_fault_last_page {0};

uint64_t CountFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (!g_report_faults) {
		return 0;
	}
	const uint64_t page = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	if (g_fault_last_page.exchange(page, std::memory_order_relaxed) == page) {
		g_fault_repeats.fetch_add(1, std::memory_order_relaxed);
	}
	const uint64_t reads =
	    (access == PageFaultAccess::Write)
	        ? g_fault_reads.load(std::memory_order_relaxed)
	        : g_fault_reads.fetch_add(1, std::memory_order_relaxed) + 1;
	const uint64_t writes =
	    (access == PageFaultAccess::Write)
	        ? g_fault_writes.fetch_add(1, std::memory_order_relaxed) + 1
	        : g_fault_writes.load(std::memory_order_relaxed);
	const uint64_t total = reads + writes;
	if (total <= 3) {
		vm_address_t              query   = page;
		vm_size_t                 length  = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t    count   = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t               object  = MACH_PORT_NULL;
		const auto                status  = vm_region_64(
            mach_task_self(), &query, &length, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
		std::fprintf(stderr,
		             "Magnus:GpuFault:Info: first fault page=0x%016llx at=0x%016llx access=%d "
		             "region=0x%016llx len=0x%llx cur=%d max=%d rc=%d\n",
		             static_cast<unsigned long long>(page),
		             static_cast<unsigned long long>(fault_vaddr), static_cast<int>(access),
		             static_cast<unsigned long long>(query),
		             static_cast<unsigned long long>(length), info.protection,
		             info.max_protection, status);
	}
	if (total % 100000 == 0) {
		std::fprintf(stderr,
		             "Magnus:GpuFault:Info: read=%llu write=%llu same_page=%llu page=0x%016llx\n",
		             static_cast<unsigned long long>(reads),
		             static_cast<unsigned long long>(writes),
		             static_cast<unsigned long long>(
		                 g_fault_repeats.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(page));
	}
	return total;
}

void ReportFaultResolution(uint64_t total, uint64_t page, uint32_t watchers) noexcept {
	if (total == 0 || (total > 3 && total % 100000 != 0)) {
		return;
	}
	vm_address_t              query   = page;
	vm_size_t                 length  = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t    count   = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t               object  = MACH_PORT_NULL;
	const auto                status  = vm_region_64(
	    mach_task_self(), &query, &length, VM_REGION_BASIC_INFO_64,
	    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	std::fprintf(stderr,
	             "Magnus:GpuFault:Info: resolved=%llu page=0x%016llx write_watchers=%u "
	             "access_watchers=%u tracker_perms=%u kernel_perms=%d rc=%d\n",
	             static_cast<unsigned long long>(total), static_cast<unsigned long long>(page),
	             watchers & 0xffu, (watchers >> 8u) & 0xffu, watchers >> 16u,
	             info.protection, status);
}

bool KernelPageAllows(PageFaultAccess access, uint64_t page) noexcept {
	vm_address_t              query   = page;
	vm_size_t                 length  = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t    count   = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t               object  = MACH_PORT_NULL;
	const auto                status  = vm_region_64(
	    mach_task_self(), &query, &length, VM_REGION_BASIC_INFO_64,
	    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	if (status != KERN_SUCCESS || query > page || length <= page - query) {
		return false;
	}
	const vm_prot_t required =
	    access == PageFaultAccess::Write ? VM_PROT_WRITE : VM_PROT_READ;
	return (info.protection & required) != 0;
}

} // namespace

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (!IsMapped(fault_vaddr, 8)) {
		return false;
	}
	if (access == PageFaultAccess::Execute) {
		return false;
	}
	const uint64_t fault_page = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const uint32_t watcher_state = m_page_manager.WatcherState(fault_page);
	const uint32_t write_watchers = watcher_state & 0xffu;
	const uint32_t access_watchers = (watcher_state >> 8u) & 0xffu;
	if ((access == PageFaultAccess::Write && write_watchers == 0 && access_watchers == 0) ||
	    (access != PageFaultAccess::Write && access_watchers == 0)) {
		if (m_page_manager.RestoreProtection(fault_page)) {
			return true;
		}
		return KernelPageAllows(access, fault_page);
	}
	const uint64_t fault_total = CountFault(access, fault_vaddr);
	(void)m_page_manager.NoteFault(fault_page);
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_page, TRACKER_PAGE_SIZE);
		m_texture_cache.InvalidateMemory(fault_page, TRACKER_PAGE_SIZE);
	} else {
		m_buffer_cache.ReadMemory(fault_page, TRACKER_PAGE_SIZE);
	}
	ReportFaultResolution(fault_total, fault_page, m_page_manager.WatcherState(fault_page));
	return true;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
	}
	m_page_manager.OnGpuMap(vaddr, size);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		if (m_scheduler.Active()) {
			const auto tick = m_scheduler.CurrentTick();
			m_scheduler.FinishCurrent();
			m_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.UnmapMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_page_manager.OnGpuUnmap(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	if (m_gpu == nullptr) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::RunGarbageCollector() {
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
