#include "IO/File.h"
#include "IO/Directory.h"
#include "IO/Path.h"
#include "IO/Crypto/Crypto.h"
#include "Misc/UTF8.h"
#include "Graphics/Utilities/TextureCompression.h"
#include "Editor/Chart/FileFormat/ComfyStudioChartFile.h"
#include "Core/ComfyStudioApplication.h"
#include "System/ComfyData.h"
#include "Render/Core/Renderer2D/Renderer2D.h"
#include <GL/glew.h>
#include <SDL.h>
#include <dlfcn.h>
#include "Misc/ImageHelper.h"
#include "Audio/Misc/SfxArchive.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace Comfy;

namespace
{
    bool CaptureEditor = false;
    Uint32 CaptureAfter = 0;
}

// Capture the full editor without adding testing hooks to production code.
extern "C" void SDLCALL SDL_GL_SwapWindow(SDL_Window* window)
{
    if (CaptureEditor && SDL_GetTicks() >= CaptureAfter)
    {
        ivec2 size;
        SDL_GL_GetDrawableSize(window, &size.x, &size.y);
        std::vector<u8> pixels(size_t(size.x) * size.y * 4);
        glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        Graphics::Utilities::FlipTextureBufferY(size, pixels.data(), Graphics::TextureFormat::RGBA8, pixels.size());
        if (!Util::WriteImage("linux-editor-smoke.png", size, pixels.data()))
            throw std::runtime_error("Cannot save the editor smoke-test screenshot");
        CaptureEditor = false;
    }
    using SwapFunction = void (*)(SDL_Window*);
    static auto swap = reinterpret_cast<SwapFunction>(dlsym(RTLD_NEXT, "SDL_GL_SwapWindow"));
    if (!swap)
        throw std::runtime_error("Cannot find SDL's swap function");
    swap(window);
}

namespace
{
    void Require(bool result, const char* message)
    {
        if (!result)
            throw std::runtime_error(message);
    }
    void TestStorageAndFormats(const std::filesystem::path& directory)
    {
        const std::string path = (directory / u8"日本語 chart.bin").u8string();
        IO::FileStream stream;
        Require(!stream.IsOpen(), "A default file stream must be closed");
        stream.CreateReadWrite(path);
        Require(stream.IsOpen() && stream.CanRead() && stream.CanWrite(), "Cannot create a Unicode file");
        Require(stream.WriteBuffer("abcdef", 6) == 6, "File write failed");
        stream.Seek(static_cast<FileAddr>(2));
        Require(stream.WriteBuffer("XY", 2) == 2, "File overwrite failed");
        Require(stream.GetLength() == static_cast<FileAddr>(6), "Overwrite changed the file size");
        stream.Close();
        Require(!stream.IsOpen() && !stream.CanRead() && !stream.CanWrite(), "Close did not clear stream state");
        Require(IO::File::ReadAllText(path) == "abXYef", "Written file contents differ");
        const std::string copy = (directory / "copy.bin").u8string();
        Require(IO::File::Copy(path, copy), "File copy failed");
        Require(!IO::File::Copy(path, copy), "Copy overwrote a file without permission");
        Require(IO::Path::GetDirectoryName("/chart.csfm") == "/", "Root directory handling is incorrect");
        Require(IO::Path::GetDirectoryName("chart.csfm").empty(), "A bare filename has a directory");
        Require(UTF8::Narrow(UTF8::Widen(u8"日本語 🐸")) == u8"日本語 🐸", "UTF-8 conversion failed");

        const std::array<u8, 16> key = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        const std::array<u8, 16> encrypted = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
        const std::array<u8, 16> expected = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        std::array<u8, 16> decrypted;
        Require(IO::Crypto::DecryptAesEcb(encrypted.data(), decrypted.data(), encrypted.size(), key), "AES decryption failed");
        Require(decrypted == expected, "AES result differs from the FIPS-197 test vector");
        Require(!IO::Crypto::DecryptAesEcb(encrypted.data(), decrypted.data(), 15, key), "AES accepted a partial block");

        Graphics::Tex texture;
        auto& mip = texture.MipMapsArray.emplace_back().emplace_back();
        mip.Size = {4, 4};
        mip.Format = Graphics::TextureFormat::DXT1;
        mip.DataSize = 8;
        mip.Data = std::make_unique<u8[]>(8);
        mip.Data[1] = 0xf8; // RGB565 red, all pixels use palette entry zero.
        const auto rgba = Graphics::Utilities::ConvertTextureToRGBA(texture);
        Require(rgba && rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255, "DXT1 decoding failed");
        const std::string dds = (directory / "texture.dds").u8string();
        Require(Graphics::Utilities::SaveTextureToDDS(dds, texture), "DDS write failed");
        Graphics::Tex restoredTexture;
        Require(Graphics::Utilities::LoadDDSToTexture(dds, restoredTexture), "DDS read failed");
        const auto restoredRGBA = Graphics::Utilities::ConvertTextureToRGBA(restoredTexture);
        Require(restoredRGBA && std::memcmp(rgba.get(), restoredRGBA.get(), 64) == 0, "DDS roundtrip changed pixels");

        Studio::Editor::Chart chart;
        chart.Properties.Song.Title = u8"Linux 日本語 chart";
        chart.SongFileName = u8"音楽/song.ogg";
        chart.Duration = TimeSpan::FromSeconds(123.5);
        Studio::Editor::ComfyStudioChartFile chartFile(chart);
        const std::string chartPath = (directory / "roundtrip.csfm").u8string();
        Require(IO::File::Save(chartPath, chartFile), "Chart serialization failed");
        auto restoredFile = IO::File::Load<Studio::Editor::ComfyStudioChartFile>(chartPath);
        Require(restoredFile != nullptr, "Chart deserialization failed");
        const auto restored = restoredFile->MoveToChart();
        Require(restored && restored->Properties.Song.Title == chart.Properties.Song.Title
            && restored->SongFileName == chart.SongFileName && restored->Duration == chart.Duration,
            "Chart roundtrip changed metadata");
    }
    void TestRendering()
    {
        System::MountComfyData();
        ApplicationHost host({});
        Render::Renderer2D renderer;
        auto target = Render::Renderer2D::CreateRenderTarget();
        target->Param.Resolution = {16, 16};
        target->Param.ClearColor = {0, 0, 0, 1};
        Render::Camera2D camera;
        camera.ProjectionSize = {16, 16};
        renderer.Begin(camera, *target);
        renderer.Draw(Render::RenderCommand2D({0, 0}, {16, 16}, {1, 0, 0, 1}));
        renderer.End();
        const auto screenshot = target->TakeScreenshot();
        const size_t center = (8 * 16 + 8) * 4;
        Require(screenshot && screenshot[center] > 250 && screenshot[center + 1] == 0
                && screenshot[center + 2] == 0, "OpenGL did not render the expected red rectangle");
        renderer.Begin(camera, *target);
        renderer.DrawRectCheckerboard({0,0}, {16,16}, {0,0}, 0, {1,1}, {1,1,1,1}, 0.25f);
        renderer.End();
        const auto checkerboard = target->TakeScreenshot();
        Require(checkerboard[0] == 0 && checkerboard[4 * 4] > 250, "Checkerboard shader did not alternate cells");
    }
    void TestGameAssets()
    {
        System::MountComfyData();
        ApplicationHost host({});
        Render::Renderer2D renderer;
        size_t textures = 0, sounds = 0;
        for (const char* name : {"spr_ps4_game", "spr_gam_cmn", "spr_fnt_36"})
        {
            const std::string path = std::string("dev_rom/2d/") + name + ".farc<" + name + ".bin>";
            auto sprites = IO::File::Load<Graphics::SprSet>(path);
            Require(sprites && !sprites->Sprites.empty(), "Could not load a supplied sprite archive");
            for (const auto& texture : sprites->TexSet.Textures)
            {
                const auto rgba = Graphics::Utilities::ConvertTextureToRGBA(*texture);
                Require(rgba != nullptr, "Could not decode a supplied game texture");
                renderer.UploadToGPUFreeCPUMemory(*texture);
                Require(ComfyTextureID(*texture).Data.ResourceView != 0, "Game texture upload failed");
                textures++;
            }
        }
        const auto fonts = IO::File::Load<Graphics::FontMap>("dev_rom/fontmap.farc<fontmap.bin>");
        Require(fonts && !fonts->Fonts.empty(), "Could not load the supplied font map");
        for (const auto& entry : std::filesystem::directory_iterator("dev_rom/sound"))
        {
            if (entry.path().extension() != ".farc")
                continue;
            Audio::SfxArchive archive(entry.path().u8string());
            const auto& entries = archive.GetEntries();
            Require(!entries.empty(), "A supplied sound bank did not load");
            for (const auto& sound : entries)
            {
                const auto source = archive.GetSource(sound);
                Require(source != Audio::SourceHandle::Invalid, "Could not decode a game sound");
                Require(Audio::AudioEngine::GetInstance().GetSharedSource(source) != nullptr, "Decoded sound has no sample data");
                sounds++;
            }
        }
        auto& audio = Audio::AudioEngine::GetInstance();
        audio.OpenStartStream();
        Require(audio.GetIsStreamOpenRunning(), "SDL audio stream did not start");
        audio.StopCloseStream();
        std::cout << "Loaded " << textures << " game textures and " << sounds << " sounds\n";
    }
    void TestEditorStartup()
    {
        Require(SDL_Init(SDL_INIT_TIMER) == 0, "Timer initialization failed");
        // Quit through the normal event path so destruction/settings saves are tested.
        CaptureEditor = true;
        CaptureAfter = SDL_GetTicks() + 1000;
        const auto timer = SDL_AddTimer(2000, [](Uint32, void*) -> Uint32
        {
            SDL_Event event = {};
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);
            return 0;
        }, nullptr);
        Require(timer != 0, "Cannot schedule smoke-test shutdown");
        Studio::ComfyStudioApplication application;
        application.Run();
        SDL_RemoveTimer(timer);
        Require(!CaptureEditor, "Editor did not render a frame before shutdown");
    }
}

int main(int argc, const char* argv[])
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--render")
            TestRendering();
        else if (argc == 2 && std::string_view(argv[1]) == "--assets")
            TestGameAssets();
        else if (argc == 2 && std::string_view(argv[1]) == "--editor")
            TestEditorStartup();
        else
        {
            char directoryTemplate[] = "/tmp/comfy-linux-tests-XXXXXX";
            const char* directory = mkdtemp(directoryTemplate);
            Require(directory != nullptr, "Cannot create a test directory");
            defer { std::filesystem::remove_all(directory); };
            TestStorageAndFormats(directory);
        }
        std::cout << "Linux tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
