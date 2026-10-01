#include "ImGui/GuiRenderer.h"
#include "ImGui/GuiRendererGlyphRanges.h"
#include "Window/ApplicationHost.h"
#include "System/ComfyData.h"
#include "OpenGL.h"
#include <SDL.h>
#include <cstddef>

#include "FontIcons.h"

using namespace Comfy;

namespace ImGui
{


	GuiRenderer::GuiRenderer(ApplicationHost& host) : host(host)
	{
	}

	bool GuiRenderer::Initialize()
	{
		if (!CreateImGuiContext())
			return false;

		if (!SetStartupIOState())
			return false;

		LoadFontFiles();
		InitializeFonts();

		if (!SetComfyStyle())
			return false;

		if (!InitializeBackend())
			return false;

		return true;
	}

	void GuiRenderer::BeginFrame()
	{
        auto& io = GetIO();
        auto* window = static_cast<SDL_Window*>(host.GetWindowHandle());
        int width, height, drawableWidth, drawableHeight;
        SDL_GetWindowSize(window, &width, &height);
        SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        io.DisplaySize = {float(width), float(height)};
        io.DisplayFramebufferScale = {width ? float(drawableWidth) / width : 1, height ? float(drawableHeight) / height : 1};
        static Uint64 previous = SDL_GetPerformanceCounter();
        const Uint64 now = SDL_GetPerformanceCounter();
        io.DeltaTime = Max(float(double(now - previous) / SDL_GetPerformanceFrequency()), 0.000001f);
        previous = now;
        int x, y;
        const Uint32 buttons = SDL_GetMouseState(&x, &y);
        io.MousePos = {float(x), float(y)};
        io.MouseDown[0] = (buttons & SDL_BUTTON_LMASK) != 0;
        io.MouseDown[1] = (buttons & SDL_BUTTON_RMASK) != 0;
        io.MouseDown[2] = (buttons & SDL_BUTTON_MMASK) != 0;
        io.KeysDown[0x01] = io.MouseDown[0];
        io.KeysDown[0x02] = io.MouseDown[1];
        io.KeysDown[0x04] = io.MouseDown[2];
        io.KeysDown[0x05] = (buttons & SDL_BUTTON_X1MASK) != 0;
        io.KeysDown[0x06] = (buttons & SDL_BUTTON_X2MASK) != 0;
        const auto modifiers = SDL_GetModState();
        io.KeyCtrl = modifiers & KMOD_CTRL;
        io.KeyShift = modifiers & KMOD_SHIFT;
        io.KeyAlt = modifiers & KMOD_ALT;
        io.KeySuper = modifiers & KMOD_GUI;
        io.KeysDown[0x10] = io.KeyShift;
        io.KeysDown[0x11] = io.KeyCtrl;
        io.KeysDown[0x12] = io.KeyAlt;
        NewFrame();
        UpdateExtendedState();
    }

	void GuiRenderer::EndFrame()
	{
        Render();
        auto* data = GetDrawData();
        const int width = int(data->DisplaySize.x * data->FramebufferScale.x);
        const int height = int(data->DisplaySize.y * data->FramebufferScale.y);
        if (width <= 0 || height <= 0)
            return;
        GLint previousArrayBuffer, previousIndexBuffer, previousProgram;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousIndexBuffer);
        // Buffers live with this ImGui context, and are released in Dispose().
        auto* buffers = static_cast<GLuint*>(GetIO().BackendRendererUserData);
        glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
        glClientActiveTexture(GL_TEXTURE0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        const auto setupRenderState = [&]()
        {
            glUseProgram(0);
            glActiveTexture(GL_TEXTURE0);
            glClientActiveTexture(GL_TEXTURE0);
            glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_SCISSOR_TEST);
            glEnable(GL_TEXTURE_2D);
            glViewport(0, 0, width, height);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(data->DisplayPos.x, data->DisplayPos.x + data->DisplaySize.x,
                    data->DisplayPos.y + data->DisplaySize.y, data->DisplayPos.y, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
        };
        setupRenderState();
        for (int listIndex = 0; listIndex < data->CmdListsCount; listIndex++)
        {
            const auto* list = data->CmdLists[listIndex];
            glBufferData(GL_ARRAY_BUFFER, list->VtxBuffer.Size * sizeof(ImDrawVert),
                         list->VtxBuffer.Data, GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, list->IdxBuffer.Size * sizeof(ImDrawIdx),
                         list->IdxBuffer.Data, GL_STREAM_DRAW);
            for (const auto& command : list->CmdBuffer)
            {
                if (command.UserCallback)
                {
                    if (command.UserCallback == ImDrawCallback_ResetRenderState)
                        setupRenderState();
                    else
                        command.UserCallback(list, &command);
                    continue;
                }
                const float left = (command.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x;
                const float top = (command.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y;
                const float right = (command.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x;
                const float bottom = (command.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y;
                glScissor(int(left), int(height - bottom), Max(0, int(right - left)), Max(0, int(bottom - top)));
                glBindTexture(GL_TEXTURE_2D, GLuint(command.TextureId.Data.ResourceView));
                const size_t base = command.VtxOffset * sizeof(ImDrawVert);
                glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                                reinterpret_cast<void*>(base + offsetof(ImDrawVert, pos)));
                glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                                  reinterpret_cast<void*>(base + offsetof(ImDrawVert, uv)));
                glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                               reinterpret_cast<void*>(base + offsetof(ImDrawVert, col)));
                const GLenum indexType = sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                glDrawElements(GL_TRIANGLES, command.ElemCount, indexType,
                               reinterpret_cast<void*>(command.IdxOffset * sizeof(ImDrawIdx)));
            }
        }
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glPopAttrib();
        glUseProgram(previousProgram);
        glPopClientAttrib();
        glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, previousIndexBuffer);
    }

	void GuiRenderer::Dispose()
	{
        auto* buffers = static_cast<GLuint*>(GetIO().BackendRendererUserData);
        glDeleteBuffers(2, buffers);
        delete[] buffers;
        GetIO().BackendRendererUserData = nullptr;
        const GLuint texture = GLuint(GetIO().Fonts->TexID.Data.ResourceView);
        glDeleteTextures(1, &texture);
        DestroyContext();
    }

	bool GuiRenderer::IsAnyViewportFocused() const
	{
        return host.IsWindowFocused();
    }

	bool GuiRenderer::CreateImGuiContext()
	{
		if (CreateContext() == nullptr)
			return false;

		return true;
	}

	bool GuiRenderer::SetStartupIOState()
	{
		auto& io = GetIO();
		io.IniFilename = ConfigFileName;
		io.LogFilename = LogFileName;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;


#if 0 // TODO: Not yet properly supported by comfy gui widgets
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
#endif

		io.KeyRepeatDelay = 0.275f;
		io.KeyRepeatRate = 0.050f;

		io.ConfigDockingNoSplit = false;
		io.ConfigDockingAlwaysTabBar = true;
		io.ConfigDockingTransparentPayload = false;

		// NOTE: Was originally enabled but causes window focus issues when spawning a file dialog from within a (technically separate win32 window) popup
		io.ConfigViewportsNoAutoMerge = false;
		// NOTE: Only makes sense for decorated windows but even then font icons result in "?" symbols everywhere
		io.ConfigViewportsNoTaskBarIcon = true;
		// NOTE: Needs to match the auto-merge inverse to not cause any ugly window popping
		io.ConfigViewportsNoDecoration = !io.ConfigViewportsNoAutoMerge;
		io.ConfigViewportsNoDefaultParent = false;

		io.ConfigWindowsMoveFromTitleBarOnly = true;

		return true;
	}

	bool GuiRenderer::LoadFontFiles()
	{
		const auto fontDirectory = System::Data.FindDirectory(FontDirectoryName);
		if (fontDirectory == nullptr)
			return false;

		const auto textFontEntry = System::Data.FindFileInDirectory(*fontDirectory, TextFontFileName);
		const auto iconFontEntry = System::Data.FindFileInDirectory(*fontDirectory, FONT_ICON_FILE_NAME_FAS);

		if (textFontEntry == nullptr || iconFontEntry == nullptr)
			return false;

		textFontFileSize = textFontEntry->Size;
		iconFontFileSize = iconFontEntry->Size;
		combinedFontFileContent = std::make_unique<u8[]>(textFontFileSize + iconFontFileSize);

		u8* textFontFileContent = combinedFontFileContent.get();
		u8* iconFontFileContent = combinedFontFileContent.get() + textFontFileSize;

		if (!System::Data.ReadFileIntoBuffer(textFontEntry, textFontFileContent) || !System::Data.ReadFileIntoBuffer(iconFontEntry, iconFontFileContent))
		{
			combinedFontFileContent = nullptr;
			return false;
		}

		return true;
	}

	bool GuiRenderer::InitializeFonts()
	{
		auto& ioFonts = *GetIO().Fonts;
		ioFonts.Flags |= ImFontAtlasFlags_NoMouseCursors;

		if (combinedFontFileContent == nullptr || textFontFileSize == 0 || iconFontFileSize == 0)
			return false;

		if (!ioFonts.Fonts.empty())
			ioFonts.Clear();

		ImFontConfig textFontConfig = {};
		textFontConfig.FontDataOwnedByAtlas = false;
		memcpy(textFontConfig.Name, TextFontName.data(), TextFontName.size());

		ImFontConfig iconFontConfig = {};
		iconFontConfig.FontDataOwnedByAtlas = false;
		iconFontConfig.GlyphMinAdvanceX = IconMinAdvanceX;
		iconFontConfig.MergeMode = true;
		memcpy(iconFontConfig.Name, IconFontName.data(), IconFontName.size());

		u8* textFontFileContent = combinedFontFileContent.get();
		u8* iconFontFileContent = combinedFontFileContent.get() + textFontFileSize;

		if (ioFonts.AddFontFromMemoryTTF(textFontFileContent, static_cast<int>(textFontFileSize), TextFontSizes[0], &textFontConfig, GetTextGlyphRange()) == nullptr)
			return false;
		if (ioFonts.AddFontFromMemoryTTF(iconFontFileContent, static_cast<int>(iconFontFileSize), IconFontSize, &iconFontConfig, GetIconGlyphRange()) == nullptr)
			return false;

#if 0 // NOTE: Additional bold fonts for fancy formatting, not needed for now
		ImFontConfig boldFontConfig = {};
		boldFontConfig.FontDataOwnedByAtlas = false;
		memcpy(boldFontConfig.Name, TextFontName.data(), TextFontName.size());

		if (ioFonts.AddFontFromMemoryTTF(textFontFileContent, static_cast<int>(textFontFileSize), TextFontSizes[1], &boldFontConfig) == nullptr)
			return false;
		if (ioFonts.AddFontFromMemoryTTF(textFontFileContent, static_cast<int>(textFontFileSize), TextFontSizes[2], &boldFontConfig) == nullptr)
			return false;
#endif

		return true;
	}

	bool GuiRenderer::SetComfyStyle()
	{
		StyleComfy();
		return true;
	}

	bool GuiRenderer::InitializeBackend()
	{
        auto& io = GetIO();
        io.BackendPlatformName = "comfy_sdl2";
        io.BackendRendererName = "comfy_opengl";
        auto* buffers = new GLuint[2]{};
        glGenBuffers(2, buffers);
        io.BackendRendererUserData = buffers;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        const int keyMap[] = {0x09, 0x25, 0x27, 0x26, 0x28, 0x21, 0x22, 0x24, 0x23, 0x2d, 0x2e, 0x08, 0x20, 0x0d, 0x1b, 0x0d, 'A', 'C', 'V', 'X', 'Y', 'Z'};
        for (int key = 0; key < ImGuiKey_COUNT; key++)
            io.KeyMap[key] = keyMap[key];
        io.SetClipboardTextFn = [](void*, const char* text) { SDL_SetClipboardText(text); };
        io.GetClipboardTextFn = [](void*) -> const char*
        {
            static std::string clipboard;
            char* text = SDL_GetClipboardText();
            clipboard = text ? text : "";
            SDL_free(text);
            return clipboard.c_str();
        };
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ComfyTextureID id;
        id.Data.ResourceView = texture;
        io.Fonts->SetTexID(id);
        SDL_StartTextInput();
        return true;
    }

	const ImWchar* GuiRenderer::GetTextGlyphRange() const
	{
		return buildFullTextGlyphRange ? TextFontGlyphRanges : GetIO().Fonts->GetGlyphRangesDefault();
	}

	const ImWchar* GuiRenderer::GetIconGlyphRange() const
	{
		static constexpr ImWchar iconFontGlyphRange[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
		return iconFontGlyphRange;
	}
}
