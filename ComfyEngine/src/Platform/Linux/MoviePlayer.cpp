#include "Render/Movie/MoviePlayer.h"
#include "Render/Core/Renderer2D/AetRenderer.h"
#include "OpenGL.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace Comfy::Render
{
    // One worker owns FFmpeg decoding; the GUI consumes completed frames and uploads textures.
    class FFmpegMoviePlayer final : public IMoviePlayer
    {
    public:
        ~FFmpegMoviePlayer() override { CloseFileAsync(); }
        bool OpenFileAsync(std::string_view path) override
        {
            CloseFileAsync();
            filePath = path;
            if (avformat_open_input(&format, filePath.c_str(), nullptr, nullptr) < 0
                || avformat_find_stream_info(format, nullptr) < 0)
            {
                CloseFileAsync();
                return false;
            }
            streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (streamIndex < 0)
            {
                CloseFileAsync();
                return false;
            }
            stream = format->streams[streamIndex];
            const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            decoder = avcodec_alloc_context3(codec);
            if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0
                || avcodec_open2(decoder, codec, nullptr) < 0)
            {
                CloseFileAsync();
                return false;
            }
            frame = av_frame_alloc();
            decodedFrame = av_frame_alloc();
            packet = av_packet_alloc();
            if (!frame || !decodedFrame || !packet)
            {
                CloseFileAsync();
                return false;
            }
            resolution = {decoder->width, decoder->height};
            duration = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0;
            auto& mip = texture.MipMapsArray.emplace_back().emplace_back();
            mip.Size = resolution;
            // FFmpeg frames are top-down, unlike the game sprite atlases.
            texture.GPU_FlipY = true;
            mip.Format = Graphics::TextureFormat::RGBA8;
            mip.DataSize = size_t(resolution.x) * resolution.y * 4;
            mip.Data = std::make_unique<u8[]>(mip.DataSize);
            sprite.PixelRegion = sprite.TexelRegion = {0, 0, resolution.x, resolution.y};
            frameRate = stream->avg_frame_rate;
            decoderThread = std::thread([this] { DecodeRequests(); });
            QueueFrame(0);
            Notify(MoviePlayerAsyncCallbackEvent::LoadedMetadata);
            Notify(MoviePlayerAsyncCallbackEvent::LoadedData);
            return true;
        }
        bool OpenFileBytesAsync(std::string_view, std::unique_ptr<u8[]>, size_t) override
        {
            // Chart movie imports use file paths; no in-memory demuxer is registered.
            return false;
        }
        bool CloseFileAsync() override
        {
            playing = false;
            {
                const auto lock = std::scoped_lock(frameMutex);
                stopping = true;
            }
            frameRequested.notify_one();
            if (decoderThread.joinable())
                decoderThread.join();
            // Join before freeing FFmpeg objects or CPU/GPU frame storage.
            {
                const auto lock = std::scoped_lock(frameMutex);
                pendingPixels.reset();
                requestedSerial = completedSerial = requestedSeekGeneration = 0;
                requestedTime = 0;
                stopping = false;
            }
            hasFrame = false;
            if (scaler)
                sws_freeContext(scaler);
            scaler = nullptr;
            av_frame_free(&frame);
            av_frame_free(&decodedFrame);
            av_packet_free(&packet);
            avcodec_free_context(&decoder);
            if (format)
                avformat_close_input(&format);
            stream = nullptr;
            streamIndex = -1;
            texture.GPU_Texture2D.Resource.reset();
            texture.MipMapsArray.clear();
            filePath.clear();
            resolution = {};
            position = duration = 0;
            decodedTime = -1;
            return true;
        }
        bool GetIsLoadingFileAsync() const override { return false; }
        std::string GetFilePath() const override { return filePath; }
        void WaitUntilFileOpenCompletedSync(TimeSpan) override {}
        bool GetIsPlaying() const override { return playing; }
        bool SetIsPlayingAsync(bool value) override
        {
            position = GetPosition().TotalSeconds();
            started = std::chrono::steady_clock::now();
            playing = value && decoder;
            return decoder != nullptr;
        }
        bool GetIsSeeking() const override
        {
            const auto lock = std::scoped_lock(frameMutex);
            return requestedSerial != completedSerial;
        }
        bool GetHasEnoughData() const override
        {
            const auto lock = std::scoped_lock(frameMutex);
            return hasFrame || pendingPixels != nullptr;
        }
        f32 GetPlaybackSpeed() const override { return speed; }
        bool SetPlaybackSpeedAsync(f32 value) override
        {
            if (!GetIsPlaybackSpeedSupported(value))
                return false;
            position = GetPosition().TotalSeconds();
            started = std::chrono::steady_clock::now();
            speed = value;
            return true;
        }
        bool GetIsScrubbing() const override { return scrubbing; }
        bool SetIsScrubbingAsync(bool value) override { scrubbing = value; return true; }
        bool GetIsPlaybackSpeedSupported(f32 value) override { return std::isfinite(value) && value > 0 && value <= 8; }
        TimeSpan GetPosition() const override
        {
            double current = position;
            if (playing)
                current += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * speed;
            if (looping && duration > 0)
                current = std::fmod(current, duration);
            return TimeSpan::FromSeconds(Clamp(current, 0.0, duration));
        }
        bool SetPositionAsync(TimeSpan value, bool accurate = true) override
        {
            if (!decoder)
                return false;
            position = Clamp(value.TotalSeconds(), 0.0, duration);
            started = std::chrono::steady_clock::now();
            QueueFrame(position, true);
            return true;
        }
        bool FrameStepAsync(bool forward) override
        {
            const double rate = av_q2d(frameRate);
            return SetPositionAsync(GetPosition() + TimeSpan::FromSeconds((forward ? 1 : -1) / (rate > 0 ? rate : 30)));
        }
        TimeSpan GetDuration() const override { return TimeSpan::FromSeconds(duration); }
        ivec2 GetResolution() const override { return resolution; }
        bool GetIsLooping() const override { return looping; }
        bool SetIsLoopingAsync(bool value) override { looping = value; return true; }
        bool GetHasVideoStream() const override { return decoder != nullptr; }
        bool GetHasAudioStream() const override { return false; }
        bool RegisterAsyncCallback(MoviePlayerAsyncCallbackFunc value) override { callback = std::move(value); return true; }
        bool TryGetStreamAttributes(MoviePlayerStreamAttributes& output) const override
        {
            if (!decoder)
                return false;
            output = {};
            output.HasVideoStream = true;
            output.PresentationDurationMFTime = u64(duration * 10000000);
            output.Video.FrameSizeWidth = resolution.x;
            output.Video.FrameSizeHeight = resolution.y;
            output.Video.FrameRateNumerator = frameRate.num;
            output.Video.FrameRateDenominator = frameRate.den;
            return true;
        }
        ComfyTextureID GetCurrentTexture() override
        {
            Update();
            return hasFrame ? ComfyTextureID(texture) : ComfyTextureID();
        }
        TexSprView GetCurrentTextureAsTexSprView() override
        {
            Update();
            return hasFrame ? TexSprView{&texture, &sprite} : TexSprView{};
        }
    private:
        int64_t StartTimestamp() const { return stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time; }
        void Notify(MoviePlayerAsyncCallbackEvent event)
        {
            if (callback)
            {
                MoviePlayerAsyncCallbackParam parameter = {};
                parameter.MoviePlayer = this;
                parameter.Event = event;
                callback(parameter);
            }
        }
        double FrameDuration() const
        {
            const double rate = av_q2d(frameRate);
            return rate > 0 ? 1.0 / rate : 1.0 / 30.0;
        }
        void QueueFrame(double target, bool explicitSeek = false)
        {
            {
                const auto lock = std::scoped_lock(frameMutex);
                // Paused redraws at the same position need no
                // additional decoding. A pending request is replaced, not queued.
                if (requestedSerial != 0 && std::abs(target - requestedTime) < 0.000001)
                    return;
                requestedTime = target;
                // Playback time advances must not cancel an in-progress seek.
                // Only an actual cursor seek invalidates the worker's result.
                if (explicitSeek)
                    requestedSeekGeneration++;
                requestedSerial++;
            }
            frameRequested.notify_one();
        }
        bool RequestWasSuperseded(u64 seekGeneration) const
        {
            const auto lock = std::scoped_lock(frameMutex);
            return stopping || requestedSeekGeneration != seekGeneration;
        }
        void Update()
        {
            if (!decoder)
                return;
            if (playing)
                QueueFrame(GetPosition().TotalSeconds());
            // The worker never touches Tex, Spr, OpenGL, or application callbacks.
            const auto lock = std::scoped_lock(frameMutex);
            if (pendingPixels)
            {
                texture.MipMapsArray[0][0].Data.swap(pendingPixels);
                pendingPixels.reset();
                texture.GPU_Texture2D.RequestReupload = true;
                hasFrame = true;
            }
        }
        void DecodeRequests()
        {
            u64 servicedSerial = 0;
            u64 servicedSeekGeneration = 0;
            while (true)
            {
                double target;
                bool explicitSeek;
                u64 serial, seekGeneration;
                {
                    auto lock = std::unique_lock(frameMutex);
                    frameRequested.wait(lock, [&] { return stopping || requestedSerial != servicedSerial; });
                    if (stopping)
                        return;
                    target = requestedTime;
                    seekGeneration = requestedSeekGeneration;
                    explicitSeek = seekGeneration != servicedSeekGeneration;
                    serial = requestedSerial;
                }
                // Nearby forward seeks can continue decoding. Rewind or jump
                // directly to a preceding keyframe for larger discontinuities.
                bool canDecode = true;
                if (decodedTime < 0 || target < decodedTime - FrameDuration()
                    || (explicitSeek && (target < decodedTime - 0.000001 || target > decodedTime + 0.5)))
                {
                    const int64_t timestamp = int64_t(target / av_q2d(stream->time_base)) + StartTimestamp();
                    canDecode = av_seek_frame(format, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD) >= 0;
                    if (canDecode)
                    {
                        avcodec_flush_buffers(decoder);
                        decodedTime = -1;
                    }
                }
                auto pixels = canDecode ? DecodeTo(target, seekGeneration) : nullptr;
                {
                    const auto lock = std::scoped_lock(frameMutex);
                    if (requestedSeekGeneration == seekGeneration && !stopping)
                    {
                        if (pixels)
                            pendingPixels = std::move(pixels);
                        completedSerial = serial;
                    }
                }
                servicedSerial = serial;
                servicedSeekGeneration = seekGeneration;
            }
        }
        std::unique_ptr<u8[]> DecodeTo(double target, u64 seekGeneration)
        {
            while (decodedTime < target)
            {
                if (RequestWasSuperseded(seekGeneration))
                    return nullptr;
                int result = avcodec_receive_frame(decoder, decodedFrame);
                if (result == AVERROR(EAGAIN))
                {
                    bool submitted = false;
                    while (true)
                    {
                        if (RequestWasSuperseded(seekGeneration))
                            return nullptr;
                        if (av_read_frame(format, packet) < 0)
                            break;
                        if (packet->stream_index == streamIndex)
                        {
                            result = avcodec_send_packet(decoder, packet);
                            submitted = result >= 0;
                        }
                        av_packet_unref(packet);
                        if (submitted)
                            break;
                    }
                    if (!submitted)
                        avcodec_send_packet(decoder, nullptr);
                    result = avcodec_receive_frame(decoder, decodedFrame);
                }
                if (result == AVERROR(EAGAIN))
                    continue;
                if (result < 0)
                    break;
                // Keep the last valid frame when receive_frame reaches EOF.
                av_frame_unref(frame);
                av_frame_move_ref(frame, decodedFrame);
                decodedTime = frame->best_effort_timestamp == AV_NOPTS_VALUE
                    ? target : double(frame->best_effort_timestamp - StartTimestamp()) * av_q2d(stream->time_base);
            }
            if (decodedTime < 0 || RequestWasSuperseded(seekGeneration))
                return nullptr;
            // Convert only the frame we will display, not every intermediate
            // frame decoded between a keyframe and the requested position.
            scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format),
                                          resolution.x, resolution.y, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scaler)
                return nullptr;
            auto pixels = std::make_unique<u8[]>(size_t(resolution.x) * resolution.y * 4);
            uint8_t* destination[] = {pixels.get()};
            int stride[] = {resolution.x * 4};
            if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, stride) <= 0)
                return nullptr;
            return pixels;
        }
        AVFormatContext* format = nullptr;
        AVCodecContext* decoder = nullptr;
        AVStream* stream = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* decodedFrame = nullptr;
        AVPacket* packet = nullptr;
        SwsContext* scaler = nullptr;
        int streamIndex = -1;
        AVRational frameRate = {};
        std::thread decoderThread;
        mutable std::mutex frameMutex;
        std::condition_variable frameRequested;
        std::unique_ptr<u8[]> pendingPixels;
        double requestedTime = 0;
        u64 requestedSerial = 0, completedSerial = 0, requestedSeekGeneration = 0;
        bool stopping = false, hasFrame = false;
        Graphics::Tex texture;
        Graphics::Spr sprite = {};
        MoviePlayerAsyncCallbackFunc callback;
        std::string filePath;
        ivec2 resolution = {};
        double position = 0, duration = 0, decodedTime = -1;
        float speed = 1;
        bool playing = false, looping = false, scrubbing = false;
        std::chrono::steady_clock::time_point started;
    };
    std::unique_ptr<IMoviePlayer> MakeD3D11MediaFoundationMediaEngineMoviePlayer()
    {
        return std::make_unique<FFmpegMoviePlayer>();
    }
}
