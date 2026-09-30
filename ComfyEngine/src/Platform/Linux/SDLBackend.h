#pragma once
#include "Audio/Core/Backend/IAudioBackend.h"
#include <SDL.h>
namespace Comfy::Audio
{
    class SDLBackend final : public IAudioBackend
    {
    public:
        ~SDLBackend() override { StopCloseStream(); }
        bool OpenStartStream(const StreamParameters& parameters, RenderCallbackFunc callback) override
        {
            StopCloseStream();
            renderCallback = std::move(callback);
            channelCount = parameters.ChannelCount;
            SDL_AudioSpec requested = {};
            requested.freq = parameters.SampleRate;
            requested.format = AUDIO_S16SYS;
            requested.channels = parameters.ChannelCount;
            requested.samples = parameters.DesiredFrameCount;
            requested.userdata = this;
            requested.callback = [](void* user, Uint8* buffer, int bytes)
            {
                auto& backend = *static_cast<SDLBackend*>(user);
                std::memset(buffer, 0, bytes);
                backend.renderCallback(reinterpret_cast<i16*>(buffer), bytes / (sizeof(i16) * backend.channelCount), backend.channelCount);
            };
            device = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
            if (!device)
                return false;
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
