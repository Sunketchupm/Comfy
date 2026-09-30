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
#include "Platform/Linux/SDLBackend.h"
#include "Platform/Linux/OpenGL.h"
#include "Editor/Chart/RenderWindow/TargetRenderHelper.h"
#include "Editor/Chart/ChartEditor.h"
#include "Editor/Chart/RenderWindow/TargetGrid.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace Comfy;

namespace
{
    bool CaptureEditor = false;
    bool RequireWayland = false;
    Uint32 CaptureAfter = 0;
}

// Capture the full editor without adding testing hooks to production code.
extern "C" void SDLCALL SDL_GL_SwapWindow(SDL_Window* window)
{
    if (CaptureEditor && SDL_GetTicks() >= CaptureAfter)
    {
        if (RequireWayland)
        {
            const char* driver = SDL_GetCurrentVideoDriver();
            if (driver == nullptr || std::string_view(driver) != "wayland")
                throw std::runtime_error("Editor did not select the native Wayland video driver");
        }
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
        Graphics::Tex atlas;
        auto& mip = atlas.MipMapsArray.emplace_back().emplace_back();
        mip.Size = {4, 4};
        mip.Format = Graphics::TextureFormat::RGBA8;
        mip.DataSize = 64;
        mip.Data = std::make_unique<u8[]>(64);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                const int offset = (y * 4 + x) * 4;
                mip.Data[offset] = x < 2 ? 255 : 0;
                mip.Data[offset + 1] = y < 2 ? 255 : 0;
                mip.Data[offset + 2] = x >= 2 && y >= 2 ? 255 : 0;
                mip.Data[offset + 3] = 255;
            }
        Render::RenderCommand2D sprite;
        sprite.TexView = &atlas;
        sprite.TexView.Filter = Graphics::TextureFilter::Point;
        // The top-right region of a bottom-up atlas contains the blue pixels.
        sprite.SourceRegion = {2, 0, 2, 2};
        sprite.Scale = {8, 8};
        renderer.Begin(camera, *target);
        renderer.Draw(sprite);
        renderer.End();
        const auto cropped = target->TakeScreenshot();
        Require(cropped[center] == 0 && cropped[center + 1] == 0 && cropped[center + 2] == 255,
                "Sprite atlas cropping sampled the wrong region");
        // Top-down sources (for example decoded movie frames) opt out of the
        // game atlas orientation. Test both halves so a whole-texture flip fails.
        sprite.SourceRegion = {2, 0, 2, 4};
        sprite.Scale = {8, 4};
        atlas.GPU_FlipY = true;
        renderer.Begin(camera, *target);
        renderer.Draw(sprite);
        renderer.End();
        const auto topDown = target->TakeScreenshot();
        const size_t upperPixel = (4 * 16 + 8) * 4;
        const size_t lowerPixel = (12 * 16 + 8) * 4;
        Require(topDown[upperPixel + 1] == 255 && topDown[upperPixel + 2] == 0
                && topDown[lowerPixel + 1] == 0 && topDown[lowerPixel + 2] == 255,
                "Top-down texture orientation is incorrect");
        atlas.GPU_FlipY = false;

        Graphics::Tex maskTexture;
        auto& maskMip = maskTexture.MipMapsArray.emplace_back().emplace_back();
        maskMip.Size = {4, 4};
        maskMip.Format = Graphics::TextureFormat::RGBA8;
        maskMip.DataSize = 64;
        maskMip.Data = std::make_unique<u8[]>(64);
        for (int pixel = 0; pixel < 16; pixel++)
        {
            std::fill(maskMip.Data.get() + pixel * 4, maskMip.Data.get() + pixel * 4 + 3, 255);
            maskMip.Data[pixel * 4 + 3] = pixel >= 8 ? 255 : 0;
        }
        auto mask = sprite;
        mask.TexView = &maskTexture;
        renderer.Begin(camera, *target);
        renderer.Draw(sprite, mask);
        renderer.End();
        const auto masked = target->TakeScreenshot();
        Require(masked[upperPixel] == 0 && masked[upperPixel + 2] == 255
                && masked[lowerPixel] == 0 && masked[lowerPixel + 1] == 0 && masked[lowerPixel + 2] == 0,
                "Masked sprites sampled the wrong texture orientation");
        renderer.Begin(camera, *target);
        renderer.DrawRectCheckerboard({0,0}, {16,16}, {0,0}, 0, {1,1}, {1,1,1,1}, 0.25f);
        renderer.End();
        const auto checkerboard = target->TakeScreenshot();
        Require(checkerboard[0] == 0 && checkerboard[4 * 4] > 250, "Checkerboard shader did not alternate cells");

        // Exercise the real grid vertex list: filled triangles can still pass
        // ordinary sprite tests, but cover far more pixels than grid lines do.
        target->Param.Resolution = {1920, 1080};
        camera.ProjectionSize = {1920, 1080};
        renderer.Begin(camera, *target);
        Studio::Editor::RenderTargetGrid(renderer, true);
        renderer.End();
        const auto grid = target->TakeScreenshot();
        const size_t pixelCount = size_t(1920) * 1080;
        size_t litPixels = 0;
        for (size_t pixel = 0; pixel < pixelCount; pixel++)
            if (grid[pixel * 4] != 0)
                litPixels++;
        Require(litPixels > 10000 && litPixels < pixelCount / 5,
                "Placement grid is missing lines or contains filled polygons");
        Require(Util::WriteImage("linux-placement-grid.png", target->Param.Resolution, grid.get()),
                "Cannot save the placement grid screenshot");
    }
    void TestChartPreview(Render::Renderer2D& renderer);
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
        TestChartPreview(renderer);
        std::cout << "Loaded " << textures << " game textures and " << sounds << " sounds\n";
    }
    void TestNavigationKeys()
    {
        const std::pair<SDL_Keycode, int> keys[] = {
            {SDLK_LEFT, 0x25}, {SDLK_UP, 0x26}, {SDLK_RIGHT, 0x27}, {SDLK_DOWN, 0x28},
            {SDLK_HOME, 0x24}, {SDLK_END, 0x23}, {SDLK_PAGEUP, 0x21}, {SDLK_PAGEDOWN, 0x22},
            {SDLK_INSERT, 0x2d}, {SDLK_DELETE, 0x2e},
            {SDLK_F1, 0x70}, {SDLK_F12, 0x7b}, {SDLK_F13, 0x7c}, {SDLK_F24, 0x87},
        };
        for (const auto& [sdlKey, editorKey] : keys)
            Require(Platform::TranslateSDLKey(sdlKey) == editorKey, "Navigation/function key translation failed");
    }
    void TestVideoSeeking(const char* path)
    {
        System::MountComfyData();
        ApplicationHost host({});
        auto movie = Render::MakeD3D11MediaFoundationMediaEngineMoviePlayer();
        Require(movie->OpenFileAsync(path), "Cannot open the video seek fixture");
        auto waitForFrame = [&]
        {
            const Uint32 deadline = SDL_GetTicks() + 10000;
            while (movie->GetIsSeeking() && SDL_GetTicks() < deadline)
                SDL_Delay(1);
            Require(!movie->GetIsSeeking(), "Video seek did not finish");
            const auto view = movie->GetCurrentTextureAsTexSprView();
            Require(view.Tex != nullptr, "Decoded video frame is missing");
            return view;
        };
        auto expectColor = [&](bool blue)
        {
            const auto view = waitForFrame();
            const auto* pixel = view.Tex->MipMapsArray[0][0].Data.get();
            Require(pixel[blue ? 2 : 0] > 200 && pixel[blue ? 0 : 2] < 30,
                    "Video seek returned a frame from the wrong position");
        };
        expectColor(false);
        const auto initialTexture = movie->GetCurrentTexture().Data.ResourceView;
        const auto started = std::chrono::steady_clock::now();
        for (int seek = 0; seek < 512; seek++)
            Require(movie->SetPositionAsync(TimeSpan::FromSeconds(seek % 2 ? 1 : 5)), "Video seek was rejected");
        movie->SetPositionAsync(TimeSpan::FromSeconds(4.5));
        const double seekMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        Require(seekMilliseconds < 500, "Rapid video seeks blocked the GUI thread");
        expectColor(true);
        Require(movie->GetCurrentTexture().Data.ResourceView == initialTexture,
                "Video frame update replaced its GPU texture instead of reusing it");
        movie->SetPositionAsync(TimeSpan::FromSeconds(0.5));
        expectColor(false);
        movie->SetPositionAsync(TimeSpan::FromSeconds(2.8));
        expectColor(false);
        movie->SetIsPlayingAsync(true);
        const Uint32 playbackDeadline = SDL_GetTicks() + 450;
        while (SDL_GetTicks() < playbackDeadline)
        {
            movie->GetCurrentTextureAsTexSprView();
            SDL_Delay(10);
        }
        movie->SetIsPlayingAsync(false);
        expectColor(true);
        movie->SetPositionAsync(TimeSpan::FromSeconds(0));
        expectColor(false);
        movie->SetPositionAsync(TimeSpan::FromSeconds(4.5));
        movie->SetIsPlayingAsync(true);
        bool reachedRequestedFrame = false;
        const Uint32 resumeDeadline = SDL_GetTicks() + 800;
        while (SDL_GetTicks() < resumeDeadline)
        {
            const auto view = movie->GetCurrentTextureAsTexSprView();
            if (view.Tex)
            {
                const auto* pixel = view.Tex->MipMapsArray[0][0].Data.get();
                reachedRequestedFrame |= pixel[2] > 200 && pixel[0] < 30;
            }
            // Keep advancing requests while the worker decodes the long GOP.
            SDL_Delay(1);
        }
        movie->SetIsPlayingAsync(false);
        Require(reachedRequestedFrame, "Starting playback cancelled the pending cursor seek");
        movie->SetPositionAsync(movie->GetDuration());
        expectColor(true);
        // Closing during a pending decode must join the worker safely.
        movie->SetPositionAsync(TimeSpan::FromSeconds(1));
        movie->CloseFileAsync();
        Require(!movie->GetHasVideoStream() && !movie->GetIsSeeking(), "Video close left a decoder request active");
        Require(movie->OpenFileAsync(path), "Cannot reopen the video after closing its worker");
        expectColor(false);
        std::cout << "Queued 512 video seeks in " << seekMilliseconds << " ms\n";
    }
    void TestAudioBuffering()
    {
        Require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "Audio initialization failed");
        defer { SDL_QuitSubSystem(SDL_INIT_AUDIO); };
        Audio::SDLBackend backend;
        // Cover the old Windows default and an arbitrary, non-power-of-two size.
        for (const u32 requestedFrames : {64u, 1500u})
        {
            std::atomic<u32> callbacks = 0;
            std::atomic<bool> validFrames = true;
            const u32 expectedFrames = requestedFrames == 64 ? 1024 : 2048;
            Audio::StreamParameters parameters = {44100, 2, requestedFrames, Audio::StreamShareMode::Shared};
            Require(backend.OpenStartStream(parameters, [&](i16* output, u32 frames, u32 channels)
            {
                if (frames != expectedFrames || channels != 2)
                    validFrames = false;
                std::fill(output, output + size_t(frames) * channels, 0);
                callbacks++;
            }), "Cannot open the audio buffering test stream");
            const Uint32 deadline = SDL_GetTicks() + 2000;
            while (callbacks.load() < 4 && SDL_GetTicks() < deadline)
                SDL_Delay(10);
            backend.StopCloseStream();
            Require(callbacks.load() >= 4, "Audio callbacks did not advance");
            Require(validFrames.load(), "SDL did not use the expected safe buffer size");
            const auto stoppedCount = callbacks.load();
            SDL_Delay(60);
            Require(callbacks.load() == stoppedCount, "Audio callbacks continued after closing the device");
        }
    }
    void TestChartPreview(Render::Renderer2D& renderer)
    {
        using namespace Studio::Editor;
        TargetRenderHelper helper;
        // Asset loading is asynchronous; keep servicing uploads on the GL thread.
        const Uint32 deadline = SDL_GetTicks() + 1500;
        while (SDL_GetTicks() < deadline)
        {
            helper.UpdateAsyncLoading(renderer);
            SDL_Delay(10);
        }
        bool fontLoaded = false;
        helper.WithFont36([&](const auto&) { fontLoaded = true; });
        Require(fontLoaded, "Preview font did not finish loading");
        helper.SetGameTheme(GameTheme::PS4FutureTone);
        helper.SetAetSprGetter(renderer);
        auto target = Render::Renderer2D::CreateRenderTarget();
        target->Param.Resolution = {1920, 1080};
        target->Param.ClearColor = {0.12f, 0.12f, 0.12f, 1};
        Render::Camera2D camera;
        camera.ProjectionSize = {1920, 1080};
        renderer.Begin(camera, *target);
        TargetRenderHelper::BackgroundData background = {};
        background.DrawGrid = true;
        background.DrawDim = true;
        helper.DrawBackground(renderer, background);
        TargetRenderHelper::HUDData hud = {};
        hud.SongTitle = u8"譜面作成";
        hud.Difficulty = Difficulty::Hard;
        hud.Duration = TimeSpan::FromSeconds(120);
        hud.DrawPracticeInfo = true;
        helper.DrawHUD(renderer, hud);
        renderer.End();
        const auto pixels = target->TakeScreenshot();
        // The life gauge is tinted green by the Aet composition. Sampling the
        // wrong atlas rows instead produces the untinted gray gauge texture.
        const size_t gaugePixel = (75 * 1920 + 400) * 4;
        Require(pixels[gaugePixel + 1] > pixels[gaugePixel] + 10
                && pixels[gaugePixel] > pixels[gaugePixel + 2] + 20,
                "The game HUD sampled an incorrect sprite atlas region");
        Require(Util::WriteImage("linux-chart-preview.png", target->Param.Resolution, pixels.get()),
                "Cannot save the chart preview screenshot");
    }
    void TestEditorStartup(bool requireWayland = false)
    {
        Require(SDL_Init(SDL_INIT_TIMER) == 0, "Timer initialization failed");
        // Quit through the normal event path so destruction/settings saves are tested.
        CaptureEditor = true;
        RequireWayland = requireWayland;
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
        if (argc == 3 && std::string_view(argv[1]) == "--video")
            TestVideoSeeking(argv[2]);
        else if (argc == 2 && std::string_view(argv[1]) == "--render")
            TestRendering();
        else if (argc == 2 && std::string_view(argv[1]) == "--audio")
            TestAudioBuffering();
        else if (argc == 2 && std::string_view(argv[1]) == "--assets")
            TestGameAssets();
        else if (argc == 2 && std::string_view(argv[1]) == "--editor")
            TestEditorStartup();
        else if (argc == 2 && std::string_view(argv[1]) == "--wayland")
            TestEditorStartup(true);
        else
        {
            char directoryTemplate[] = "/tmp/comfy-linux-tests-XXXXXX";
            const char* directory = mkdtemp(directoryTemplate);
            Require(directory != nullptr, "Cannot create a test directory");
            defer { std::filesystem::remove_all(directory); };
            TestStorageAndFormats(directory);
            TestNavigationKeys();
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
