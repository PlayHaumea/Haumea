#ifdef __APPLE__

#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

std::atomic<unsigned long long> imagesAdded{0};
std::atomic<unsigned long long> imagesRemoved{0};

const bool countingImages = [] {
    _dyld_register_func_for_add_image([](const mach_header*, intptr_t) { imagesAdded.fetch_add(1); });
    _dyld_register_func_for_remove_image([](const mach_header*, intptr_t) { imagesRemoved.fetch_add(1); });
    return true;
}();

constexpr std::uint64_t MetaVersion = 2;
constexpr std::size_t LoadBiasOffset = 0x08;
constexpr std::size_t HeaderCountOffset = 0x58;
constexpr std::size_t HeadersOffset = 0x60;

// A main executable that was not relinked (a host test, for example) is reported from its Mach-O
// segments, as Linux reports any executable's program headers.
std::vector<Elf64_Phdr> SegmentHeaders(const mach_header_64* header) {
    std::vector<Elf64_Phdr> headers;
    const auto* command = reinterpret_cast<const load_command*>(header + 1);
    for (std::uint32_t index = 0; index < header->ncmds; ++index) {
        if (command->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if (segment->vmsize != 0 && segment->initprot != 0) {
                Elf64_Phdr phdr{};
                phdr.p_type = 1;
                phdr.p_flags = ((segment->initprot & VM_PROT_READ) ? 4u : 0u) | ((segment->initprot & VM_PROT_WRITE) ? 2u : 0u) | ((segment->initprot & VM_PROT_EXECUTE) ? 1u : 0u);
                phdr.p_offset = segment->fileoff;
                phdr.p_vaddr = segment->vmaddr;
                phdr.p_paddr = segment->vmaddr;
                phdr.p_filesz = segment->filesize;
                phdr.p_memsz = segment->vmsize;
                phdr.p_align = 0x1000;
                headers.push_back(phdr);
            }
        }
        command = reinterpret_cast<const load_command*>(reinterpret_cast<const std::uint8_t*>(command) + command->cmdsize);
    }
    return headers;
}

}

extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data) {
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(index));
        if (header == nullptr || header->magic != MH_MAGIC_64 || (header->flags & MH_DYLIB_IN_CACHE) != 0) continue;
        unsigned long size = 0;
        const auto* meta = getsectiondata(header, "__APS5DATA", "__meta", &size);
        if ((meta == nullptr || size < HeadersOffset) && header->filetype == MH_EXECUTE) {
            static const std::vector<Elf64_Phdr> segments = SegmentHeaders(header);
            dl_phdr_info info {};
            info.dlpi_addr = static_cast<std::uintptr_t>(_dyld_get_image_vmaddr_slide(index));
            info.dlpi_name = "";
            info.dlpi_phdr = segments.data();
            info.dlpi_phnum = static_cast<std::uint16_t>(segments.size());
            info.dlpi_adds = imagesAdded.load();
            info.dlpi_subs = imagesRemoved.load();
            if (const int result = callback(&info, sizeof(info), data); result != 0) return result;
            continue;
        }
        if (meta == nullptr || size < HeadersOffset) continue;
        std::uint64_t version;
        std::uint64_t loadBias;
        std::uint64_t count;
        std::memcpy(&version, meta, sizeof(version));
        std::memcpy(&loadBias, meta + LoadBiasOffset, sizeof(loadBias));
        std::memcpy(&count, meta + HeaderCountOffset, sizeof(count));
        if (version != MetaVersion || count > 0xffff || count > (size - HeadersOffset) / sizeof(Elf64_Phdr)) continue;
        dl_phdr_info info {};
        info.dlpi_addr = static_cast<std::uintptr_t>(loadBias);
        info.dlpi_name = header->filetype == MH_EXECUTE ? "" : _dyld_get_image_name(index);
        info.dlpi_phdr = reinterpret_cast<const Elf64_Phdr*>(meta + HeadersOffset);
        info.dlpi_phnum = static_cast<std::uint16_t>(count);
        info.dlpi_adds = imagesAdded.load();
        info.dlpi_subs = imagesRemoved.load();
        // A TLS module id per image with PT_TLS: its dyld index, which is stable while it stays loaded.
        for (std::uint64_t h = 0; h < count; ++h)
            if (info.dlpi_phdr[h].p_type == 7 /* PT_TLS */ && info.dlpi_phdr[h].p_memsz != 0) info.dlpi_tls_modid = index + 1;
        if (const int result = callback(&info, sizeof(info), data); result != 0) return result;
    }
    return 0;
}

#endif
