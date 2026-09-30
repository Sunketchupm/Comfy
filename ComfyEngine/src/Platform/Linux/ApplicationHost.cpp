#include "Window/ApplicationHost.h"
#include "ImGui/GuiRenderer.h"
#include "Audio/Core/AudioEngine.h"
#include "Input/Core/InputSystem.h"
#include "OpenGL.h"
#include <SDL.h>
#include <stdexcept>

namespace Comfy::Platform
{
    int TranslateSDLKey(int key)
    {
        if (key >= SDLK_a && key <= SDLK_z)
            return key - SDLK_a + 'A';
        if (key >= SDLK_0 && key <= SDLK_9)
            return key;
        // SDL's F13-F24 codes are separated from F1-F12 by navigation
        // keys. Treating that entire range as function keys swallowed arrows.
        if (key >= SDLK_F1 && key <= SDLK_F12)
            return key - SDLK_F1 + 0x70;
        if (key >= SDLK_F13 && key <= SDLK_F24)
            return key - SDLK_F13 + 0x7c;
        switch (key)
        {
        case SDLK_TAB: return 0x09;
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d;
        case SDLK_ESCAPE: return 0x1b;
        case SDLK_SPACE: return 0x20;
        case SDLK_PAGEUP: return 0x21;
        case SDLK_PAGEDOWN: return 0x22;
        case SDLK_END: return 0x23;
        case SDLK_HOME: return 0x24;
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_INSERT: return 0x2d;
        case SDLK_DELETE: return 0x2e;
        case SDLK_SEMICOLON: return 0xba;
        case SDLK_EQUALS: return 0xbb;
        case SDLK_COMMA: return 0xbc;
        case SDLK_MINUS: return 0xbd;
        case SDLK_PERIOD: return 0xbe;
        case SDLK_SLASH: return 0xbf;
        case SDLK_BACKQUOTE: return 0xc0;
        case SDLK_LEFTBRACKET: return 0xdb;
        case SDLK_BACKSLASH: return 0xdc;
        case SDLK_RIGHTBRACKET: return 0xdd;
        case SDLK_QUOTE: return 0xde;
        default: return 0;
        }
    }
}
namespace Comfy
{
    struct ApplicationHost::Impl
    {
        SDL_Window* Window = nullptr;
        SDL_GLContext Context = nullptr;
        Gui::GuiRenderer Renderer;
        std::string Title;
        bool Running = true, Fullscreen = false, Sleep = false;
        bool Focused = true, Gained = false, Lost = false;
        int SwapInterval = 1;
        ivec4 RestoreRegion = {0, 0, 1280, 720};
        std::vector<std::string> DroppedFiles;
        bool DropDispatched = true;
        std::function<void(ivec2)> Resize;
        std::function<ApplicationHostCloseResponse()> Closing;
        std::function<void()> Destroy;
        Impl(ApplicationHost& host) : Renderer(host) {}
    };
    ApplicationHost::ApplicationHost(const ConstructionParam& parameters) : impl(std::make_unique<Impl>(*this))
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
            throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        const auto& state = parameters.StartupWindowState;
        const auto size = state.Size.value_or(DefaultStartupWindowSize);
        const auto position = state.Position.value_or(ivec2(SDL_WINDOWPOS_CENTERED));
        impl->Title = state.Title.value_or(UnnamedWindowName);
        impl->Window = SDL_CreateWindow(impl->Title.c_str(), position.x, position.y, size.x, size.y,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!impl->Window)
            throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(impl->Window, WindowSizeRestraints.x, WindowSizeRestraints.y);
        impl->Context = SDL_GL_CreateContext(impl->Window);
        if (!impl->Context)
            throw std::runtime_error(SDL_GetError());
        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        // GLEW may report that GLX is unavailable for SDL's EGL/offscreen context
        // after successfully loading the OpenGL entry points.
        if (glewStatus != GLEW_OK && (!glGenFramebuffers || !glBindFramebuffer))
            throw std::runtime_error(reinterpret_cast<const char*>(glewGetErrorString(glewStatus)));
        TimeSpan::InitializeClock();
        if (!impl->Renderer.Initialize())
            throw std::runtime_error("Could not initialize the GUI");
        Input::GlobalSystemInitialize(impl->Window);
        Audio::AudioEngine::CreateInstance();
        SetSwapInterval(1);
        if (state.RestoreRegion)
            SetWindowRestoreRegion(*state.RestoreRegion);
        if (state.IsMaximized.value_or(false))
            SetIsMaximized(true);
        if (state.IsFullscreen.value_or(false))
            SetIsFullscreen(true);
    }
    ApplicationHost::~ApplicationHost()
    {
        Audio::AudioEngine::DeleteInstance();
        Input::GlobalSystemDispose(impl->Window);
        impl->Renderer.Dispose();
        SDL_GL_DeleteContext(impl->Context);
        SDL_DestroyWindow(impl->Window);
        SDL_Quit();
    }
    void ApplicationHost::EnterProgramLoop(std::function<void()> update)
    {
        TimeSpan previous = TimeSpan::GetTimeNow();
        while (impl->Running)
        {
            impl->Gained = impl->Lost = false;
            if (impl->DropDispatched)
                impl->DroppedFiles.clear();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                auto& io = Gui::GetIO();
                switch (event.type)
                {
                case SDL_QUIT:
                    if (!impl->Closing || impl->Closing() == ApplicationHostCloseResponse::Exit)
                        impl->Running = false;
                    break;
                case SDL_KEYDOWN: case SDL_KEYUP:
                {
                    const int key = Platform::TranslateSDLKey(event.key.keysym.sym);
                    if (key > 0 && key < IM_ARRAYSIZE(io.KeysDown))
                        io.KeysDown[key] = event.type == SDL_KEYDOWN;
                    break;
                }
                case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
                    Input::GlobalSystemRefreshDevices();
                    break;
                case SDL_TEXTINPUT: io.AddInputCharactersUTF8(event.text.text); break;
                case SDL_MOUSEWHEEL:
                    io.MouseWheel += float(event.wheel.y);
                    io.MouseWheelH += float(event.wheel.x);
                    break;
                case SDL_DROPFILE:
                    impl->DroppedFiles.emplace_back(event.drop.file);
                    SDL_free(event.drop.file);
                    impl->DropDispatched = false;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && impl->Resize)
                        impl->Resize({event.window.data1, event.window.data2});
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    {
                        impl->Focused = true;
                        impl->Gained = true;
                    }
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    {
                        impl->Focused = false;
                        impl->Lost = true;
                        std::fill(std::begin(io.KeysDown), std::end(io.KeysDown), false);
                    }
                    break;
                }
            }
            if (!impl->Running)
                break;
            const auto now = TimeSpan::GetTimeNow();
            impl->Renderer.BeginFrame();
            Input::GlobalSystemUpdateFrame(now - previous, impl->Focused);
            previous = now;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.16f, 0.16f, 0.16f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            update();
            impl->Renderer.EndFrame();
            SDL_GL_SwapWindow(impl->Window);
            if (impl->Sleep || !impl->Focused)
                SDL_Delay(5);
        }
        if (impl->Destroy)
            impl->Destroy();
    }
    void ApplicationHost::GuiMainDockspace(bool menu)
    {
        const auto* viewport = Gui::GetMainViewport();
        Gui::SetNextWindowPos(viewport->WorkPos);
        Gui::SetNextWindowSize(viewport->WorkSize);
        Gui::SetNextWindowViewport(viewport->ID);
        Gui::PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(0));
        Gui::Begin("##MainDockspace", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
        Gui::PopStyleVar();
        Gui::DockSpace(Gui::GetID(Gui::GuiRenderer::MainDockSpaceID), vec2(0), ImGuiDockNodeFlags_PassthruCentralNode);
        Gui::End();
    }
    void ApplicationHost::Exit() { impl->Running = false; }
    std::string_view ApplicationHost::GetWindowTitle() const { return impl->Title; }
    void ApplicationHost::SetWindowTitle(std::string_view value)
    {
        impl->Title = value;
        SDL_SetWindowTitle(impl->Window, impl->Title.c_str());
    }
    bool ApplicationHost::GetIsFullscreen() const { return impl->Fullscreen; }
    void ApplicationHost::SetIsFullscreen(bool value)
    {
        if (value && !impl->Fullscreen)
            impl->RestoreRegion = {GetWindowPosition(), GetWindowSize()};
        if (SDL_SetWindowFullscreen(impl->Window, value ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
            impl->Fullscreen = value;
    }
    void ApplicationHost::ToggleFullscreen() { SetIsFullscreen(!GetIsFullscreen()); }
    bool ApplicationHost::GetIsMaximized() const { return SDL_GetWindowFlags(impl->Window) & SDL_WINDOW_MAXIMIZED; }
    void ApplicationHost::SetIsMaximized(bool value)
    {
        if (value)
            SDL_MaximizeWindow(impl->Window);
        else
            SDL_RestoreWindow(impl->Window);
    }
    int ApplicationHost::GetSwapInterval() const { return impl->SwapInterval; }
    void ApplicationHost::SetSwapInterval(int value)
    {
        if (SDL_GL_SetSwapInterval(value) == 0)
            impl->SwapInterval = value;
    }
    bool ApplicationHost::IsWindowFocused(bool) const { return impl->Focused; }
    bool ApplicationHost::HasFocusBeenGained(bool) const { return impl->Gained; }
    bool ApplicationHost::HasFocusBeenLost(bool) const { return impl->Lost; }
    bool ApplicationHost::GetMainLoopPowerSleep() const { return impl->Sleep; }
    void ApplicationHost::SetMainLoopPowerSleep(bool value) { impl->Sleep = value; }
    ivec2 ApplicationHost::GetWindowPosition() const
    {
        ivec2 position;
        SDL_GetWindowPosition(impl->Window, &position.x, &position.y);
        return position;
    }
    void ApplicationHost::SetWindowPosition(ivec2 value) { SDL_SetWindowPosition(impl->Window, value.x, value.y); }
    ivec2 ApplicationHost::GetWindowSize() const
    {
        ivec2 size;
        SDL_GetWindowSize(impl->Window, &size.x, &size.y);
        return size;
    }
    void ApplicationHost::SetWindowSize(ivec2 value) { SDL_SetWindowSize(impl->Window, value.x, value.y); }
    ivec4 ApplicationHost::GetWindowRestoreRegion()
    {
        return impl->Fullscreen || GetIsMaximized() ? impl->RestoreRegion : ivec4(GetWindowPosition(), GetWindowSize());
    }
    void ApplicationHost::SetWindowRestoreRegion(ivec4 value) { impl->RestoreRegion = value; }
    void* ApplicationHost::GetWindowHandle() const { return impl->Window; }
    void ApplicationHost::RegisterWindowProcCallback(std::function<bool(HWND, UINT, WPARAM, LPARAM)>) {}
    void ApplicationHost::RegisterWindowResizeCallback(std::function<void(ivec2)> value) { impl->Resize = std::move(value); }
    void ApplicationHost::RegisterWindowClosingCallback(std::function<ApplicationHostCloseResponse()> value) { impl->Closing = std::move(value); }
    void ApplicationHost::RegisterWindowDestoyCallback(std::function<void()> value) { impl->Destroy = std::move(value); }
    bool ApplicationHost::GetDispatchFileDrop() { return !impl->DropDispatched; }
    void ApplicationHost::SetFileDropDispatched(bool value) { impl->DropDispatched = value; }
    const std::vector<std::string>& ApplicationHost::GetDroppedFiles() const { return impl->DroppedFiles; }
    HICON ApplicationHost::GetComfyWindowIcon() { return nullptr; }
}
