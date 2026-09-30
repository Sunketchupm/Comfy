#pragma once
#include "Audio/Core/Backend/IAudioBackend.h"
#include <SDL.h>
#include <algorithm>
namespace Comfy::Audio
{
    class SDLBackend final : public IAudioBackend
    {
    public:
        ~SDLBackend() override { StopCloseStream(); }
        bool OpenStartStream(const StreamParameters& parameters, RenderCallbackFunc callback) override
        {
            StopCloseStream();
            if (!callback || parameters.SampleRate == 0 || parameters.ChannelCount == 0 || parameters.ChannelCount > 255)
                return false;

            renderCallback = std::move(callback);
            channelCount = parameters.ChannelCount;
            SDL_AudioSpec requested = {};
            requested.freq = parameters.SampleRate;
            requested.format = AUDIO_S16SYS;
            requested.channels = parameters.ChannelCount;
            // Tiny Windows buffers can force the shared Linux audio graph to run
            // too frequently. Keep enough headroom for desktop scheduling jitter.
            // SDL expects a power of two; 32768 also fits its Uint16 sample count
            // and the engine's 44100-frame mixing buffer.
            u32 bufferFrames = 128u;
            requested.samples = static_cast<Uint16>(bufferFrames);
            requested.userdata = this;
            requested.callback = [](void* user, Uint8* buffer, int bytes)
            {
                auto& backend = *static_cast<SDLBackend*>(user);
                std::memset(buffer, 0, bytes);
                backend.renderCallback(reinterpret_cast<i16*>(buffer), bytes / (sizeof(i16) * backend.channelCount), backend.channelCount);
            };
            device = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
            if (!device)
            {
                renderCallback = {};
                return false;
            }
            SDL_PauseAudioDevice(device, 0);
            return true;
        }
        bool StopCloseStream() override
        {
            if (device)
                SDL_CloseAudioDevice(device);
            device = 0;
            renderCallback = {};
            return true;
        }
        bool IsOpenRunning() const override { return device != 0; }
    private:
        SDL_AudioDeviceID device = 0;
        u32 channelCount = 0;
        RenderCallbackFunc renderCallback;
    };
}
