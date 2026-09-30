#include "Render/Movie/MoviePlayer.h"
#include "Render/Core/Renderer2D/AetRenderer.h"
#include "OpenGL.h"
#include <chrono>
#include <cmath>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace Comfy::Render
{
    // FFmpeg owns the demuxer/decoder. All texture uploads stay on the GUI thread.
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
            packet = av_packet_alloc();
            if (!frame || !packet)
            {
                CloseFileAsync();
                return false;
            }
            resolution = {decoder->width, decoder->height};
            duration = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0;
            auto& mip = texture.MipMapsArray.emplace_back().emplace_back();
            mip.Size = resolution;
            mip.Format = Graphics::TextureFormat::RGBA8;
            mip.DataSize = size_t(resolution.x) * resolution.y * 4;
            mip.Data = std::make_unique<u8[]>(mip.DataSize);
            sprite.PixelRegion = sprite.TexelRegion = {0, 0, resolution.x, resolution.y};
            DecodeTo(0);
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
            if (scaler)
                sws_freeContext(scaler);
            scaler = nullptr;
            av_frame_free(&frame);
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
        bool GetIsSeeking() const override { return false; }
        bool GetHasEnoughData() const override { return decoder != nullptr; }
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
            const int64_t timestamp = int64_t(position / av_q2d(stream->time_base)) + StartTimestamp();
            if (av_seek_frame(format, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0)
                return false;
            avcodec_flush_buffers(decoder);
            decodedTime = -1;
            return DecodeTo(position);
        }
        bool FrameStepAsync(bool forward) override
        {
            const double rate = av_q2d(stream->avg_frame_rate);
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
            output.Video.FrameRateNumerator = stream->avg_frame_rate.num;
            output.Video.FrameRateDenominator = stream->avg_frame_rate.den;
            return true;
        }
        ComfyTextureID GetCurrentTexture() override
        {
            Update();
            return decoder ? ComfyTextureID(texture) : ComfyTextureID();
        }
        TexSprView GetCurrentTextureAsTexSprView() override
        {
            Update();
            return decoder ? TexSprView{&texture, &sprite} : TexSprView{};
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
        void Update()
        {
            if (!decoder)
                return;
            const double target = GetPosition().TotalSeconds();
            if (target + 0.1 < decodedTime)
                SetPositionAsync(TimeSpan::FromSeconds(target));
            else if (target > decodedTime)
                DecodeTo(target);
        }
        bool DecodeTo(double target)
        {
            while (decodedTime < target)
            {
                int result = avcodec_receive_frame(decoder, frame);
                if (result == AVERROR(EAGAIN))
                {
                    bool submitted = false;
                    while (av_read_frame(format, packet) >= 0)
                    {
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
                    result = avcodec_receive_frame(decoder, frame);
                }
                if (result < 0)
                    return decodedTime >= 0;
                decodedTime = frame->best_effort_timestamp == AV_NOPTS_VALUE
                    ? target : double(frame->best_effort_timestamp - StartTimestamp()) * av_q2d(stream->time_base);
                scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format),
                                              resolution.x, resolution.y, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!scaler)
                    return false;
                auto& mip = texture.MipMapsArray[0][0];
                uint8_t* destination[] = {mip.Data.get()};
                int stride[] = {resolution.x * 4};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, stride);
                texture.GPU_Texture2D.RequestReupload = true;
            }
            return true;
        }
        AVFormatContext* format = nullptr;
        AVCodecContext* decoder = nullptr;
        AVStream* stream = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* packet = nullptr;
        SwsContext* scaler = nullptr;
        int streamIndex = -1;
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
