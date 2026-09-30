#include "IO/Shell.h"
#include "IO/Path.h"
#include <SDL.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
extern char** environ;

namespace Comfy::IO::Shell
{
    namespace
    {
        // Pass arguments directly: paths and titles never go through a shell.
        bool Run(const std::vector<std::string>& arguments, std::string* output = nullptr)
        {
            std::vector<char*> argv;
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            int descriptors[2];
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            if (output)
            {
                if (pipe(descriptors) != 0)
                    return false;
                posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
                posix_spawn_file_actions_addclose(&actions, descriptors[0]);
                posix_spawn_file_actions_addclose(&actions, descriptors[1]);
            }
            pid_t process;
            const int result = posix_spawnp(&process, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (output)
            {
                close(descriptors[1]);
                if (result == 0)
                {
                    char buffer[4096];
                    ssize_t count;
                    while ((count = read(descriptors[0], buffer, sizeof(buffer))) > 0)
                        output->append(buffer, count);
                }
                close(descriptors[0]);
            }
            if (result != 0)
                return false;
            int status;
            while (waitpid(process, &status, 0) < 0)
                if (errno != EINTR)
                    return false;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    bool IsFileLink(std::string path)
    {
        std::error_code error;
        return std::filesystem::is_symlink(std::filesystem::u8path(path), error);
    }
    std::string ResolveFileLink(std::string_view path)
    {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(std::filesystem::u8path(path), error);
        return error ? std::string(path) : resolved.u8string();
    }
    void OpenWithDefaultProgram(std::string_view path) { Run({"xdg-open", Path::ResolveRelative(path)}); }
    void OpenInExplorer(std::string_view path)
    {
        OpenWithDefaultProgram(Path::IsDirectory(path) ? path : Path::GetDirectoryName(path));
    }
    void OpenExplorerProperties(std::string_view path) { OpenInExplorer(path); }
    MessageBoxResult ShowMessageBox(std::string_view message, std::string_view title,
                                   MessageBoxButtons buttons, MessageBoxIcon icon, void* parent)
    {
        std::vector<SDL_MessageBoxButtonData> choices;
        auto add = [&](const char* text, MessageBoxResult result, Uint32 flags = 0)
        {
            choices.push_back({flags, int(result), text});
        };
        switch (buttons)
        {
        case MessageBoxButtons::YesNo:
        case MessageBoxButtons::YesNoCancel:
            add("Yes", MessageBoxResult::Yes, SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
            add("No", MessageBoxResult::No);
            if (buttons == MessageBoxButtons::YesNoCancel)
                add("Cancel", MessageBoxResult::Cancel, SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
            break;
        case MessageBoxButtons::RetryCancel:
            add("Retry", MessageBoxResult::Retry);
            add("Cancel", MessageBoxResult::Cancel, SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
            break;
        case MessageBoxButtons::AbortRetryIgnore:
            add("Abort", MessageBoxResult::Abort);
            add("Retry", MessageBoxResult::Retry);
            add("Ignore", MessageBoxResult::Ignore);
            break;
        case MessageBoxButtons::CancelTryContinue:
            add("Cancel", MessageBoxResult::Cancel);
            add("Try again", MessageBoxResult::TryAgain);
            add("Continue", MessageBoxResult::Continue);
            break;
        default:
            add("OK", MessageBoxResult::OK, SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT);
            if (buttons == MessageBoxButtons::OKCancel)
                add("Cancel", MessageBoxResult::Cancel, SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT);
        }
        const std::string messageText(message), titleText(title);
        SDL_MessageBoxData data = {};
        data.flags = icon == MessageBoxIcon::Error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION;
        data.window = static_cast<SDL_Window*>(parent);
        data.title = titleText.c_str();
        data.message = messageText.c_str();
        data.numbuttons = choices.size();
        data.buttons = choices.data();
        int selected = int(MessageBoxResult::None);
        SDL_ShowMessageBox(&data, &selected);
        return static_cast<MessageBoxResult>(selected);
    }
    bool FileDialog::OpenRead() { return InternalCreateAndShowDialog(false, false); }
    bool FileDialog::OpenSave() { return InternalCreateAndShowDialog(true, false); }
    bool FileDialog::OpenSelectFolder() { return InternalCreateAndShowDialog(false, true); }
    bool FileDialog::InternalCreateAndShowDialog(bool save, bool folder)
    {
        std::vector<std::string> arguments = { "zenity", "--file-selection", "--title=" + std::string(Title) };
        if (save)
        {
            arguments.push_back("--save");
            arguments.push_back("--confirm-overwrite");
        }
        if (folder)
            arguments.push_back("--directory");
        if (!FileName.empty())
            arguments.push_back("--filename=" + FileName);
        for (const auto& filter : Filters)
        {
            std::string patterns = filter.Spec;
            std::replace(patterns.begin(), patterns.end(), ';', ' ');
            if (patterns == "*.*")
                patterns = "*";
            arguments.push_back("--file-filter=" + filter.Name + " | " + patterns);
        }
        std::string selected;
        if (!Run(arguments, &selected))
            return false;
        if (!selected.empty() && selected.back() == '\n')
            selected.pop_back();
        if (selected.empty())
            return false;
        if (save && !DefaultExtension.empty() && Path::GetExtension(selected).empty())
            selected += DefaultExtension.front() == '.' ? DefaultExtension : "." + DefaultExtension;
        OutFilePath = std::move(selected);
        return true;
    }
}
