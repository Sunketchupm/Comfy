#include "System/Library/LibraryLoader.h"
#include "IO/Path.h"
#include <dlfcn.h>
namespace Comfy::System
{
    LibraryLoader::LibraryLoader(std::string_view name, bool loadOnInit) : libraryName(name), moduleHandle(nullptr)
    {
        if (loadOnInit)
            Load();
    }
    bool LibraryLoader::Load(std::string_view directory, bool directoryOnly)
    {
        if (moduleHandle)
            return true;
        if (!directory.empty())
            moduleHandle = dlopen(IO::Path::Combine(directory, libraryName).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!moduleHandle && !directoryOnly)
            moduleHandle = dlopen(libraryName.c_str(), RTLD_NOW | RTLD_LOCAL);
        return moduleHandle != nullptr;
    }
    void LibraryLoader::UnLoad()
    {
        if (moduleHandle)
            dlclose(moduleHandle);
        moduleHandle = nullptr;
    }
    bool LibraryLoader::GetLibraryLoaded() const { return moduleHandle != nullptr; }
    void* LibraryLoader::GetFunctionAddress(const char* name) const
    {
        return moduleHandle ? dlsym(moduleHandle, name) : nullptr;
    }
}
