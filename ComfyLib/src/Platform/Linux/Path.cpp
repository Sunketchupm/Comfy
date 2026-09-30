#include "IO/Path.h"
#include "IO/Directory.h"
#include "Misc/UTF8.h"
#include "Misc/StringUtil.h"
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

namespace Comfy::IO
{
	namespace Path
	{
		bool DoesAnyPackedExtensionMatch(std::string_view extensionToCheck, std::string_view packedExtensions, char packedSeparator)
		{
			// NOTE: Invalid packed extension
			assert(!packedExtensions.empty() && packedExtensions.back() != packedSeparator);

			if (extensionToCheck.size() > packedExtensions.size())
				return false;

			size_t lastSeparatorIndex = 0;
			for (size_t i = 0; i < packedExtensions.size(); i++)
			{
				const bool isLastCharacter = ((i + 1) == packedExtensions.size());

				if (packedExtensions[i] == packedSeparator || isLastCharacter)
				{
					const size_t separatorIndex = i;
					const size_t extensionSize = isLastCharacter ?
						(separatorIndex - packedExtensions.size()) :
						(separatorIndex - lastSeparatorIndex);

					const bool isFirstExtension = (lastSeparatorIndex == 0);
					const auto subString = isFirstExtension ?
						packedExtensions.substr(lastSeparatorIndex, extensionSize) :
						packedExtensions.substr(lastSeparatorIndex + 1, extensionSize - 1);

					lastSeparatorIndex = separatorIndex;

					if (Util::MatchesInsensitive(extensionToCheck, subString))
						return true;
				}
			}

			return false;
		}

		bool IsRelative(std::string_view filePath)
		{
        return std::filesystem::u8path(filePath).is_relative();
    }

		bool IsDirectory(std::string_view filePath)
		{
        return Directory::Exists(filePath);
    }

		std::string ResolveRelative(std::string_view relativePath)
		{
			if (relativePath.empty() || !IsRelative(relativePath))
				return std::string(relativePath);

			return Combine(Directory::GetWorkingDirectory(), relativePath);
		}

		std::string ResolveRelativeTo(std::string_view relativePath, std::string_view baseFileOrDirectory)
		{
			if (relativePath.empty() || baseFileOrDirectory.empty() || !IsRelative(relativePath))
				return std::string(relativePath);

			const auto baseDirectory = IsDirectory(baseFileOrDirectory) ? baseFileOrDirectory : GetDirectoryName(baseFileOrDirectory);
			return Combine(baseDirectory, relativePath);
		}

		std::string TryMakeRelative(std::string_view absolutePath, std::string_view baseFileOrDirectory)
		{
        const auto base = IsDirectory(baseFileOrDirectory) ? baseFileOrDirectory : GetDirectoryName(baseFileOrDirectory);
        const auto relative = std::filesystem::u8path(absolutePath).lexically_relative(std::filesystem::u8path(base));
        return relative.empty() ? std::string(absolutePath) : relative.u8string();
    }

		std::string ChangeExtension(std::string_view filePath, std::string_view newExtension)
		{
			if (const auto archivePath = Archive::ParsePath(filePath); !archivePath.FileName.empty())
				return IO::Archive::CombinePath(archivePath.BasePath, ChangeExtension(archivePath.FileName, newExtension));

			const auto oldExtension = GetExtension(filePath);

			std::string newPath;
			newPath.reserve(filePath.size() - oldExtension.size() + newExtension.size());
			newPath += filePath.substr(0, filePath.size() - oldExtension.size());
			newPath += newExtension;
			return newPath;
		}

		std::string Combine(std::string_view basePath, std::string_view fileOrDirectory)
		{
			const auto trimmedBase = TrimTrailingPathSeparators(basePath);
			std::string combinedPath;
			combinedPath.reserve(trimmedBase.size() + fileOrDirectory.size() + 1);
			combinedPath += trimmedBase;
			combinedPath += DirectorySeparator;
			combinedPath += fileOrDirectory;
			return combinedPath;
		}

		std::string Normalize(std::string_view filePath)
		{
			auto normalized = std::string(filePath);
			std::replace(normalized.begin(), normalized.end(), DirectorySeparatorAlt, DirectorySeparator);
			return normalized;
		}

		std::string NormalizeWin32(std::string_view filePath)
		{
			auto normalized = std::string(filePath);
			std::replace(normalized.begin(), normalized.end(), DirectorySeparator, DirectorySeparatorAlt);
			return normalized;
		}

		std::string TrySearchSystemSearchPath(std::string_view fileName, std::string_view fileExtension)
		{
        // Linux executable names do not use a .exe suffix.
        const char* environment = std::getenv("PATH");
        if (environment == nullptr)
            return {};
        const std::string_view searchPath(environment);
        size_t start = 0;
        do
        {
            const size_t end = searchPath.find(':', start);
            const auto directory = searchPath.substr(start, end == std::string_view::npos ? end : end - start);
            const auto candidate = Combine(directory.empty() ? "." : directory, fileName);
            if (access(candidate.c_str(), X_OK) == 0 && File::Exists(candidate))
                return candidate;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        } while (start <= searchPath.size());
        return {};
    }
	}
}

// #define COMFY_STATIC_ASSERT_STRCMP(stringA, stringB) static_assert((stringA) == (stringB))
#define COMFY_STATIC_ASSERT_STRCMP(stringA, stringB) static_assert(Util::Matches((stringA), (stringB)))

namespace Comfy::IO::Path::ConstexprTest
{
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/MyDir/MySubDir/MyFile"), "");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/MyDir/MySubDir"), "");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/MyDir/MyFile.ext"), ".ext");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/MyDir/MyFile."), ".");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/MyFile.ext"), ".ext");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("MyFile.ext"), ".ext");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("MyFile"), "");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/.ext"), ".ext");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension("C:/"), "");
	COMFY_STATIC_ASSERT_STRCMP(GetExtension(""), "");

	COMFY_STATIC_ASSERT_STRCMP(GetFileName("C:/MyDir/MySubDir/MyFile.ext"), "MyFile.ext");

	COMFY_STATIC_ASSERT_STRCMP(TrimTrailingPathSeparators("C:/MyDir/MySubDir/"), "C:/MyDir/MySubDir");
	COMFY_STATIC_ASSERT_STRCMP(TrimTrailingPathSeparators("C:/MyDir//"), "C:/MyDir");
	COMFY_STATIC_ASSERT_STRCMP(TrimTrailingPathSeparators("C:/MyDir"), "C:/MyDir");
	COMFY_STATIC_ASSERT_STRCMP(TrimTrailingPathSeparators(""), "");

	COMFY_STATIC_ASSERT_STRCMP(GetDirectoryName("C:/MyDir/MySubDir/MyFile.ext"), "C:/MyDir/MySubDir");
	COMFY_STATIC_ASSERT_STRCMP(GetDirectoryName("C:/MyDir/MySubDir"), "C:/MyDir");
	COMFY_STATIC_ASSERT_STRCMP(GetDirectoryName("C:/MyDir/"), "C:/MyDir/");
	COMFY_STATIC_ASSERT_STRCMP(GetDirectoryName("C:/MyDir"), "C:");
	COMFY_STATIC_ASSERT_STRCMP(GetDirectoryName("C:/"), "C:/");
}
