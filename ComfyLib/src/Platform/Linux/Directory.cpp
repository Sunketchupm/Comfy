#include "IO/Directory.h"
#include <system_error>

namespace Comfy::IO::Directory
{
    bool Exists(std::string_view path)
    {
        std::error_code error;
        return std::filesystem::is_directory(std::filesystem::u8path(path), error);
    }
    bool Create(std::string_view path)
    {
        std::error_code error;
        return std::filesystem::create_directory(std::filesystem::u8path(path), error);
    }
    bool CreateRecursive(std::string_view path)
    {
        std::error_code error;
        return std::filesystem::create_directories(std::filesystem::u8path(path), error);
    }
    std::string GetWorkingDirectory()
    {
        return std::filesystem::current_path().u8string();
    }
    void SetWorkingDirectory(std::string_view path)
    {
        std::filesystem::current_path(std::filesystem::u8path(path));
    }
    std::string GetExecutableDirectory()
    {
        return std::filesystem::read_symlink("/proc/self/exe").parent_path().u8string();
    }
}
