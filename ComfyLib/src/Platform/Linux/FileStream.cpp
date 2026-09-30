#include "IO/Stream/FileStream.h"
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace Comfy::IO
{
    namespace
    {
        FILE* OpenExisting(std::string_view path, int flags, const char* mode)
        {
            const std::string terminatedPath(path);
            const int descriptor = open(terminatedPath.c_str(), flags);
            if (descriptor < 0)
                return nullptr;
            FILE* stream = fdopen(descriptor, mode);
            if (stream == nullptr)
                close(descriptor);
            return stream;
        }
    }
    FileStream::FileStream(FileStream&& other)
    {
        canRead = other.canRead;
        canWrite = other.canWrite;
        position = other.position;
        fileSize = other.fileSize;
        fileHandle = other.fileHandle;
        other.fileHandle = nullptr;
        other.canRead = other.canWrite = false;
        other.position = other.fileSize = {};
    }
    FileStream::~FileStream() { Close(); }
    void FileStream::Seek(FileAddr target)
    {
        if (IsOpen() && fseeko(static_cast<FILE*>(fileHandle), static_cast<off_t>(target), SEEK_SET) == 0)
            position = target;
    }
    FileAddr FileStream::GetPosition() const { return position; }
    FileAddr FileStream::GetLength() const { return fileSize; }
    bool FileStream::IsOpen() const { return fileHandle != nullptr; }
    bool FileStream::CanRead() const { return canRead; }
    bool FileStream::CanWrite() const { return canWrite; }
    size_t FileStream::ReadBuffer(void* buffer, size_t size)
    {
        if (!CanRead())
            return 0;
        const size_t count = fread(buffer, 1, size, static_cast<FILE*>(fileHandle));
        position += static_cast<FileAddr>(count);
        return count;
    }
    size_t FileStream::WriteBuffer(const void* buffer, size_t size)
    {
        if (!CanWrite())
            return 0;
        const size_t count = fwrite(buffer, 1, size, static_cast<FILE*>(fileHandle));
        position += static_cast<FileAddr>(count);
        if (position > fileSize)
            fileSize = position;
        return count;
    }
    void FileStream::OpenRead(std::string_view path)
    {
        Close();
        fileHandle = OpenExisting(path, O_RDONLY, "rb");
        canRead = IsOpen();
        UpdateFileSize();
    }
    void FileStream::OpenWrite(std::string_view path)
    {
        Close();
        fileHandle = OpenExisting(path, O_WRONLY, "wb");
        canWrite = IsOpen();
        UpdateFileSize();
    }
    void FileStream::OpenReadWrite(std::string_view path)
    {
        Close();
        fileHandle = OpenExisting(path, O_RDWR, "r+b");
        canRead = canWrite = IsOpen();
        UpdateFileSize();
    }
    void FileStream::CreateWrite(std::string_view path)
    {
        Close();
        fileHandle = fopen(std::string(path).c_str(), "wb");
        canWrite = IsOpen();
        UpdateFileSize();
    }
    void FileStream::CreateReadWrite(std::string_view path)
    {
        Close();
        fileHandle = fopen(std::string(path).c_str(), "w+b");
        canRead = canWrite = IsOpen();
        UpdateFileSize();
    }
    void FileStream::Close()
    {
        if (IsOpen())
            fclose(static_cast<FILE*>(fileHandle));
        fileHandle = nullptr;
        canRead = canWrite = false;
        position = fileSize = {};
    }
    void FileStream::UpdateFileSize()
    {
        struct stat status = {};
        if (IsOpen() && fstat(fileno(static_cast<FILE*>(fileHandle)), &status) == 0)
            fileSize = static_cast<FileAddr>(status.st_size);
    }
}
