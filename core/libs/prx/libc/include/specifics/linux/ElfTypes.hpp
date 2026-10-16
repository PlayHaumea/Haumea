#ifndef PRX_LIBC_INCLUDE_SPECIFICS_LINUX_ELFTYPES_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_LINUX_ELFTYPES_HPP

#include <cstdint>
#include <cstddef>

struct Elf64_Phdr {
    std::uint32_t p_type;
    std::uint32_t p_flags;
    std::uint64_t p_offset;
    std::uint64_t p_vaddr;
    std::uint64_t p_paddr;
    std::uint64_t p_filesz;
    std::uint64_t p_memsz;
    std::uint64_t p_align;
};

struct dl_phdr_info {
    std::uintptr_t dlpi_addr;
    const char* dlpi_name;
    const Elf64_Phdr* dlpi_phdr;
    std::uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;   // images loaded so far
    unsigned long long dlpi_subs;   // images unloaded so far
    std::size_t dlpi_tls_modid;     // the image's TLS module id, 0 without TLS
    void* dlpi_tls_data;
};

static constexpr std::uint32_t PT_LOAD = 1;
static constexpr std::uint32_t PT_DYNAMIC = 2;
static constexpr std::uint32_t PT_TLS = 7;

struct Elf64_Dyn {
    std::int64_t d_tag;
    union {
        std::uint64_t d_val;
        std::uint64_t d_ptr;
    } d_un;
};

static constexpr std::int64_t DT_NULL = 0;
static constexpr std::int64_t DT_INIT = 12;
static constexpr std::int64_t DT_FINI = 13;

#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif
static constexpr std::uint32_t PT_GNU_EH_FRAME = 0x6474e550;
static constexpr std::uint32_t PF_X = 1;
static constexpr std::uint32_t PF_R = 4;
static constexpr std::uint32_t PF_W = 2;

#ifdef __APPLE__
extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data) __asm__("_dl_iterate_phdr_nid_no_patch");
#else
extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data);
#endif

#endif
