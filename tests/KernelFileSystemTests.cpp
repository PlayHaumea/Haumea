#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

extern "C" uint64_t FEXCompiledBlockCount() { return 0; }
extern "C" uint64_t FEXFrontendCompileMicroseconds() { return 0; }
extern "C" uint64_t FEXBackendCompileMicroseconds() { return 0; }
extern "C" bool StingerFixUnalignedFault(void *) { return false; }

namespace Magnus {
uint64_t UnalignedAccessCount() { return 0; }
uint64_t UnalignedAccessCount(uint32_t) { return 0; }
uint64_t GuestFaultCount(uint32_t) { return 0; }
}

namespace {

namespace FileSystem = Libs::LibKernel::FileSystem;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "KernelFileSystemTests: failed: %s\n", text);
    std::abort();
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("kyty_kernel_file_system_" + std::to_string(unique));
    Check(std::filesystem::create_directories(m_path),
          "create temporary directory");
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] const std::filesystem::path &Path() const { return m_path; }

  KYTY_CLASS_NO_COPY(TempDirectory);

private:
  std::filesystem::path m_path;
};

void CheckSaveRename(const std::filesystem::path &root,
                     std::string_view payload) {
  constexpr char Source[] = "/savedata0/STEMP000.DAT";
  constexpr char Target[] = "/savedata0/SDATA000.DAT";
  constexpr char Suffix[] = "-after-rename";

  const int fd = FileSystem::KernelOpen(Source, 0x601, 0777);
  Check(fd >= 3, "open temporary save file");
  Check(FileSystem::KernelWrite(fd, payload.data(), payload.size()) ==
            payload.size(),
        "write save payload");
  Check(FileSystem::KernelRename(Source, Target) == OK,
        "rename open save file");
  Check(FileSystem::KernelWrite(fd, Suffix, sizeof(Suffix) - 1) ==
            sizeof(Suffix) - 1,
        "write through renamed descriptor");
  Check(FileSystem::KernelClose(fd) == OK, "close renamed descriptor");

  FileSystem::FileStat file_stat {};
  Check(FileSystem::KernelStat(Target, &file_stat) == OK, "stat renamed save file");
  Check((file_stat.st_mode & 0170000u) == 0100000u, "renamed save is a file");
  Check(file_stat.st_size == static_cast<int64_t>(payload.size() + sizeof(Suffix) - 1),
        "renamed save stat size");

  Common::File result(root / "SDATA000.DAT", Common::File::Mode::Read);
  Check(!result.IsInvalid(), "open renamed save file");
  const auto data = result.ReadWholeBuffer();
  result.Close();
  const std::string expected = std::string(payload) + Suffix;
  Check(data.Size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.GetData(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

} // namespace

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::OutputDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  Libs::Graphics::SetAppPaused(false);
  Check(!Libs::Graphics::IsAppPaused(), "active app presentation state");
  Libs::Graphics::SetAppPaused(true);
  Check(Libs::Graphics::IsAppPaused(), "inactive app presentation state");
  Libs::Graphics::SetAppPaused(false);

  TempDirectory temporary;
  FileSystem::Initialize();
  FileSystem::Mount(temporary.Path(), "/savedata0");
  FileSystem::FileStat directory_stat {};
  Check(FileSystem::KernelStat("/savedata0", &directory_stat) == OK,
        "stat mounted directory");
  Check((directory_stat.st_mode & 0170000u) == 0040000u,
        "mounted path is a directory");
  Check(FileSystem::KernelStat("/savedata0/missing", &directory_stat) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT,
        "missing path returns not found");
#if defined(__APPLE__)
  Check(FileSystem::KernelMkdir("guestfs/BedrockLevelInfoCache/cache-entry", 0777) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT,
        "cache child reports missing parent");
#endif
  Check(FileSystem::KernelMkdir("guestfs/BedrockLevelInfoCache", 0777) == OK,
        "create cache parent");
  Check(FileSystem::KernelMkdir("guestfs/BedrockLevelInfoCache/cache-entry", 0777) == OK,
        "create cache child");
  Check(FileSystem::KernelStat("guestfs/BedrockLevelInfoCache/cache-entry", &directory_stat) == OK,
        "stat cache child");
  Check((directory_stat.st_mode & 0170000u) == 0040000u,
        "cache child is a directory");
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
