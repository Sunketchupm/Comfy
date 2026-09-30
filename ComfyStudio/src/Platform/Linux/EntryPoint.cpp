#include "Core/ComfyStudioApplication.h"
#include "CLI/CommandLine.h"
#include "IO/Directory.h"
#include "IO/Path.h"
#include <iostream>
int main(int argc, const char* argv[])
{
    using namespace Comfy;
    try
    {
        if (Studio::CLI::CommandLine::Parse(argc, argv) == Studio::CLI::CommandLineResult::Exit)
            return EXIT_SUCCESS;
        const std::string fileToOpen = argc == 2 ? IO::Path::ResolveRelative(argv[1]) : "";
        IO::Directory::SetWorkingDirectory(IO::Directory::GetExecutableDirectory());
        Studio::ComfyStudioApplication application(fileToOpen);
        application.Run();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Comfy Studio: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
