#include "Graphics/Utilities/TextureCompression.h"
#include "IO/Path.h"
#include "Misc/UTF8.h"


#include "BlockCompression.h"



namespace Comfy::Graphics::Utilities
{

	size_t TextureFormatBlockSize(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::DXT1:
		case TextureFormat::DXT1a:
		case TextureFormat::RGTC1:
			return 8;

		case TextureFormat::DXT3:
		case TextureFormat::DXT5:
		case TextureFormat::RGTC2:
			return 16;

		default:
			return 0;
		}
	}

	size_t TextureFormatChannelCount(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::A8:
		case TextureFormat::L8:
		case TextureFormat::RGTC1:
			return 1;

		case TextureFormat::L8A8:
		case TextureFormat::RGTC2:
			return 2;

		case TextureFormat::RGB8:
		case TextureFormat::RGB5:
		case TextureFormat::DXT1:
			return 3;

		case TextureFormat::RGBA8:
		case TextureFormat::RGB5_A1:
		case TextureFormat::RGBA4:
		case TextureFormat::DXT1a:
		case TextureFormat::DXT3:
		case TextureFormat::DXT5:
			return 4;

		default:
			assert(false);
			return 0;
		}
	}

	size_t TextureFormatByteSize(ivec2 size, TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::A8:
		case TextureFormat::L8:
		case TextureFormat::RGB8:
		case TextureFormat::RGBA8:
		case TextureFormat::L8A8:
			return size.x * size.y * TextureFormatChannelCount(format);

			// BUG: Not entirely accurate but not like these are used anyway
		case TextureFormat::RGB5:
			assert(false);
			return (size.x * size.y * 15) / CHAR_BIT;
		case TextureFormat::RGB5_A1:
			assert(false);
			return (size.x * size.y * 16) / CHAR_BIT;

		case TextureFormat::RGBA4:
			return (size.x * size.y * TextureFormatChannelCount(format)) / 2;

		case TextureFormat::DXT1:
		case TextureFormat::DXT1a:
		case TextureFormat::DXT3:
		case TextureFormat::DXT5:
		case TextureFormat::RGTC1:
		case TextureFormat::RGTC2:
			return Max(1, (size.x + 3) / 4) * Max(1, (size.y + 3) / 4) * TextureFormatBlockSize(format);

		default:
			assert(false);
			return 0;
		}
	}

	bool DecompressTextureData(ivec2 size, const u8* inData, TextureFormat inFormat, size_t inByteSize, u8* outData, TextureFormat outFormat, size_t outByteSize)
	{
        if (size.x <= 0 || size.y <= 0 || !inData || !outData || outFormat != TextureFormat::RGBA8)
            return false;
        if (inByteSize < TextureFormatByteSize(size, inFormat) || outByteSize < size_t(size.x) * size.y * 4)
            return false;
        if (TextureFormatBlockSize(inFormat) != 0)
            return DecodeBlocks(size, inData, inFormat, outData);
        for (size_t pixel = 0; pixel < size_t(size.x) * size.y; pixel++)
        {
            u8* output = outData + pixel * 4;
            const size_t channels = TextureFormatChannelCount(inFormat);
            const u8* input = inData + pixel * channels;
            if (inFormat == TextureFormat::RGBA8)
                std::memcpy(output, input, 4);
            else if (inFormat == TextureFormat::RGB8)
            {
                std::memcpy(output, input, 3);
                output[3] = 255;
            }
            else if (inFormat == TextureFormat::A8)
            {
                output[0] = output[1] = output[2] = 255;
                output[3] = input[0];
            }
            else if (inFormat == TextureFormat::L8 || inFormat == TextureFormat::L8A8)
            {
                output[0] = output[1] = output[2] = input[0];
                output[3] = channels == 2 ? input[1] : 255;
            }
            else
                return false;
        }
        return true;
    }

	bool CompressTextureData(ivec2 size, const u8* inData, TextureFormat inFormat, size_t inByteSize, u8* outData, TextureFormat outFormat, size_t outByteSize)
	{
        if (!inData || !outData || size.x <= 0 || size.y <= 0 || inFormat != TextureFormat::RGBA8)
            return false;
        if (inByteSize < size_t(size.x) * size.y * 4 || outByteSize < TextureFormatByteSize(size, outFormat))
            return false;
        if (outFormat == TextureFormat::RGBA8)
        {
            std::memcpy(outData, inData, size_t(size.x) * size.y * 4);
            return true;
        }
        if (outFormat != TextureFormat::DXT1 && outFormat != TextureFormat::DXT5)
            return false;
        for (int y = 0; y < size.y; y += 4)
        {
            for (int x = 0; x < size.x; x += 4)
            {
                u8 block[64];
                for (int row = 0; row < 4; row++)
                    for (int column = 0; column < 4; column++)
                        std::memcpy(block + (row * 4 + column) * 4,
                            inData + (Min(y + row, size.y - 1) * size.x + Min(x + column, size.x - 1)) * 4, 4);
                EncodeColorBlock(block, outData, outFormat == TextureFormat::DXT5);
                outData += TextureFormatBlockSize(outFormat);
            }
        }
        return true;
    }

	namespace
	{
		constexpr float CbCrOffset = 0.503929f;
		constexpr float CbCrFactor = 1.003922f;

		constexpr mat3 YCbCrToRGBTransform =
		{
			vec3(+1.5748f, +1.0f, +0.0000f),
			vec3(-0.4681f, +1.0f, -0.1873f),
			vec3(+0.0000f, +1.0f, +1.8556f),
		};

		constexpr mat3 RGBToYCbCrTransform =
		{
			vec3(+0.500004232f, -0.454162151f, -0.0458420813f),
			vec3(+0.212593317f, +0.715214610f, +0.0721921176f),
			vec3(-0.114568502f, -0.385435730f, +0.5000042320f),
		};

		constexpr float PixelU8ToF32(u8 pixel)
		{
			constexpr auto factor = 1.0f / static_cast<float>(std::numeric_limits<u8>::max());
			return static_cast<float>(pixel) * factor;
		}

		constexpr u8 PixelF32ToU8(float pixel)
		{
			constexpr auto factor = static_cast<float>(std::numeric_limits<u8>::max());
			return static_cast<u8>(Clamp(pixel, 0.0f, 1.0f) * factor);
		}

		constexpr u32 PackU8RGBA(u8 r, u8 g, u8 b, u8 a)
		{
			return
				(static_cast<u32>(a) << 24) |
				(static_cast<u32>(b) << 16) |
				(static_cast<u32>(g) << 8) |
				(static_cast<u32>(r) << 0);
		}

		inline u32 ConvertSinglePixelYACbCrToRGBA(const u8 inYA[2], const u8 inCbCr[2])
		{
			const auto yCbCr = vec3(
				(PixelU8ToF32(inCbCr[1]) * CbCrFactor) - CbCrOffset,
				(PixelU8ToF32(inYA[0])),
				(PixelU8ToF32(inCbCr[0]) * CbCrFactor) - CbCrOffset);

			return PackU8RGBA(
				PixelF32ToU8(glm::dot(yCbCr, YCbCrToRGBTransform[0])),
				PixelF32ToU8(glm::dot(yCbCr, YCbCrToRGBTransform[1])),
				PixelF32ToU8(glm::dot(yCbCr, YCbCrToRGBTransform[2])),
				inYA[1]);
		}

		inline void ConvertSinglePixelRGBAToYACbCr(const u32 inRGBA, u8 outYA[2], u8 outCbCr[2])
		{
			constexpr float cbCrFactor = 1.0f / CbCrFactor;

			const auto rgb = vec3(
				PixelU8ToF32((inRGBA & 0x0000FF)),
				PixelU8ToF32((inRGBA & 0x00FF00) >> 8),
				PixelU8ToF32((inRGBA & 0xFF0000) >> 16));

			outCbCr[0] = PixelF32ToU8((glm::dot(rgb, RGBToYCbCrTransform[2]) + CbCrOffset) * cbCrFactor);
			outCbCr[1] = PixelF32ToU8((glm::dot(rgb, RGBToYCbCrTransform[0]) + CbCrOffset) * cbCrFactor);

			outYA[0] = PixelF32ToU8(glm::dot(rgb, RGBToYCbCrTransform[1]));
			outYA[1] = static_cast<u8>(inRGBA >> 24);
		}
	}

	bool ConvertYACbCrToRGBABuffer(const TexMipMap& mipMapYA, const TexMipMap& mipMapCbCr, u8* outData, size_t outByteSize)
	{
        if (mipMapYA.Format != TextureFormat::RGTC2 || mipMapCbCr.Format != TextureFormat::RGTC2
            || outByteSize < size_t(mipMapYA.Size.x) * mipMapYA.Size.y * 4)
            return false;
        std::vector<u8> ya(size_t(mipMapYA.Size.x) * mipMapYA.Size.y * 4);
        std::vector<u8> cbcr(size_t(mipMapCbCr.Size.x) * mipMapCbCr.Size.y * 4);
        if (!DecompressTextureData(mipMapYA.Size, mipMapYA.Data.get(), mipMapYA.Format, mipMapYA.DataSize,
                                  ya.data(), TextureFormat::RGBA8, ya.size())
            || !DecompressTextureData(mipMapCbCr.Size, mipMapCbCr.Data.get(), mipMapCbCr.Format, mipMapCbCr.DataSize,
                                      cbcr.data(), TextureFormat::RGBA8, cbcr.size()))
            return false;
        for (int y = 0; y < mipMapYA.Size.y; y++)
            for (int x = 0; x < mipMapYA.Size.x; x++)
            {
                const size_t pixel = (y * mipMapYA.Size.x + x) * 4;
                const size_t chroma = (Min(y / 2, mipMapCbCr.Size.y - 1) * mipMapCbCr.Size.x
                                      + Min(x / 2, mipMapCbCr.Size.x - 1)) * 4;
                const u32 color = ConvertSinglePixelYACbCrToRGBA(ya.data() + pixel, cbcr.data() + chroma);
                std::memcpy(outData + pixel, &color, 4);
            }
        return true;
    }

	bool ConvertRGBAToYACbCrBuffer(ivec2 size, const u8* inData, TextureFormat inFormat, size_t inByteSize, u8* outYAData, u8* outCbCrData)
	{
		if (inFormat != TextureFormat::RGBA8)
			return false;

		const auto inRGBAData = reinterpret_cast<const u32*>(inData);

		for (size_t y = 0; y < size.y; y++)
		{
			for (size_t x = 0; x < size.x; x++)
			{
				const auto pixelIndex = (size.x * y + x);

				u8* outYA = &outYAData[pixelIndex * 2];
				u8* outCbCr = &outCbCrData[pixelIndex * 2];

				ConvertSinglePixelRGBAToYACbCr(inRGBAData[pixelIndex], outYA, outCbCr);
			}
		}

		return true;
	}

	bool CreateYACbCrTexture(ivec2 size, const u8* inData, TextureFormat inFormat, size_t inByteSize, Tex& outTexture)
	{
        return false;
    }

	bool ConvertTextureToRGBABuffer(const Tex& inTexture, u8* outData, size_t outByteSize, i32 cubeFace)
	{
		if (inTexture.MipMapsArray.empty() || cubeFace >= inTexture.MipMapsArray.size())
			return false;

		const auto& mips = inTexture.MipMapsArray[cubeFace];
		if (mips.empty())
			return false;

		const auto& frontMip = mips.front();
		const bool decodeYACbCr = (inTexture.GetSignature() == TxpSig::Texture2D && mips.size() == 2 && frontMip.Format == TextureFormat::RGTC2);

		if (decodeYACbCr)
			return ConvertYACbCrToRGBABuffer(mips[0], mips[1], outData, outByteSize);

		return DecompressTextureData(frontMip.Size, frontMip.Data.get(), frontMip.Format, frontMip.DataSize, outData, TextureFormat::RGBA8, outByteSize);
	}

	std::unique_ptr<u8[]> ConvertTextureToRGBA(const Tex& inTexture, i32 cubeFace)
	{
		const auto outByteSize = TextureFormatByteSize(inTexture.GetSize(), TextureFormat::RGBA8);
		auto outData = std::make_unique<u8[]>(outByteSize);

		if (!ConvertTextureToRGBABuffer(inTexture, outData.get(), outByteSize, cubeFace))
			return nullptr;

		return outData;
	}

	bool FlipTextureBufferY(ivec2 size, u8* inOutData, TextureFormat inFormat, size_t inByteSize)
	{
		if (size.x <= 0 || size.y <= 0)
			return false;

		if (inFormat != TextureFormat::RGBA8)
			return false;

		if (inByteSize < TextureFormatByteSize(size, inFormat))
			return false;

		auto inOutRGBAPixels = reinterpret_cast<u32*>(inOutData);

		for (auto y = 0; y < size.y / 2; y++)
		{
			for (auto x = 0; x < size.x; x++)
			{
				u32& pixel = inOutRGBAPixels[size.x * y + x];
				u32& flippedPixel = inOutRGBAPixels[(size.x * (size.y - 1 - y)) + x];

				std::swap(pixel, flippedPixel);
			}
		}

		return true;
	}

	bool ResizeTextureBuffer(ivec2 inSize, const u8* inData, TextureFormat inFormat, size_t inByteSize, ivec2 outSize, u8* outData, size_t outByteSize, FilterMode filterMode)
	{
        if (!inData || !outData || inSize.x <= 0 || inSize.y <= 0 || outSize.x <= 0 || outSize.y <= 0)
            return false;
        if (TextureFormatBlockSize(inFormat) != 0)
            return false;
        const size_t channels = TextureFormatChannelCount(inFormat);
        if (inByteSize < size_t(inSize.x) * inSize.y * channels || outByteSize < size_t(outSize.x) * outSize.y * channels)
            return false;
        for (int y = 0; y < outSize.y; y++)
        {
            for (int x = 0; x < outSize.x; x++)
            {
                const float sourceX = Max(0.0f, (x + 0.5f) * inSize.x / outSize.x - 0.5f);
                const float sourceY = Max(0.0f, (y + 0.5f) * inSize.y / outSize.y - 0.5f);
                const int left = Min(int(sourceX), inSize.x - 1);
                const int top = Min(int(sourceY), inSize.y - 1);
                for (size_t channel = 0; channel < channels; channel++)
                {
                    auto sample = [&](int sx, int sy) { return float(inData[(sy * inSize.x + sx) * channels + channel]); };
                    float value = sample(left, top);
                    if (filterMode != FilterMode::Point)
                    {
                        const int right = Min(left + 1, inSize.x - 1);
                        const int bottom = Min(top + 1, inSize.y - 1);
                        const float horizontal = sourceX - left;
                        const float vertical = sourceY - top;
                        value = glm::mix(glm::mix(sample(left, top), sample(right, top), horizontal),
                                         glm::mix(sample(left, bottom), sample(right, bottom), horizontal), vertical);
                    }
                    outData[(y * outSize.x + x) * channels + channel] = u8(value + 0.5f);
                }
            }
        }
        return true;
    }

	bool ConvertRGBToRGBA(ivec2 size, const u8* inData, size_t inByteSize, u8* outData, size_t outByteSize)
	{
		if (size.x <= 0 || size.y <= 0)
			return false;

		const auto expectedInputByteSize = TextureFormatByteSize(size, TextureFormat::RGB8);
		if (inByteSize < expectedInputByteSize)
			return false;

		const auto expectedOutputByteSize = TextureFormatByteSize(size, TextureFormat::RGBA8);
		if (outByteSize < expectedOutputByteSize)
			return false;

		const u8* currentRGBPixel = inData;
		u32* outRGBAPixel = reinterpret_cast<u32*>(outData);

		for (size_t i = 0; i < (size.x * size.y); i++)
		{
			const u8 r = currentRGBPixel[0];
			const u8 g = currentRGBPixel[1];
			const u8 b = currentRGBPixel[2];
			const u8 a = std::numeric_limits<u8>::max();
			currentRGBPixel += 3;

			outRGBAPixel[i] = (r << 0) | (g << 8) | (b << 16) | (a << 24);
		}

		return true;
	}

	bool LoadDDSToTexture(std::string_view filePath, Tex& outTexture)
	{
        const auto [content, fileSize] = IO::File::ReadAllBytes(filePath);
        if (!content || fileSize < 128 || std::memcmp(content.get(), "DDS ", 4) != 0)
            return false;
        u32 header[31];
        std::memcpy(header, content.get() + 4, sizeof(header));
        if (header[0] != 124 || header[18] != 32 || header[2] == 0 || header[3] == 0
            || header[2] > 16384 || header[3] > 16384 || header[6] > 15 || (header[27] & 0x200000))
            return false;
        auto fourCC = [](const char* value)
        {
            u32 result;
            std::memcpy(&result, value, 4);
            return result;
        };
        TextureFormat format;
        if (header[19] & 4)
        {
            if (header[20] == fourCC("DXT1")) format = TextureFormat::DXT1;
            else if (header[20] == fourCC("DXT3")) format = TextureFormat::DXT3;
            else if (header[20] == fourCC("DXT5")) format = TextureFormat::DXT5;
            else if (header[20] == fourCC("ATI1") || header[20] == fourCC("BC4U")) format = TextureFormat::RGTC1;
            else if (header[20] == fourCC("ATI2") || header[20] == fourCC("BC5U")) format = TextureFormat::RGTC2;
            else return false;
        }
        else if ((header[19] & 0x40) && header[21] == 32 && header[23] == 0xff00
                 && (header[22] == 0xff || header[22] == 0xff0000))
            format = TextureFormat::RGBA8;
        else
            return false;
        std::vector<std::vector<TexMipMap>> faces;
        const int faceCount = (header[27] & 0x200) ? 6 : 1;
        size_t offset = 128;
        for (int face = 0; face < faceCount; face++)
        {
            auto& mips = faces.emplace_back();
            ivec2 size(header[3], header[2]);
            for (u32 level = 0; level < Max(header[6], 1u); level++)
            {
                const size_t byteSize = TextureFormatByteSize(size, format);
                if (byteSize > fileSize - offset)
                    return false;
                TexMipMap mip;
                mip.Size = size;
                mip.Format = format;
                mip.DataSize = byteSize;
                mip.Data = std::make_unique<u8[]>(byteSize);
                std::memcpy(mip.Data.get(), content.get() + offset, byteSize);
                if (format == TextureFormat::RGBA8)
                    for (size_t pixel = 0; pixel < byteSize; pixel += 4)
                    {
                        if (header[22] == 0xff0000)
                            std::swap(mip.Data[pixel], mip.Data[pixel + 2]);
                        if (header[25] == 0)
                            mip.Data[pixel + 3] = 255;
                    }
                offset += byteSize;
                mips.push_back(std::move(mip));
                size = Max(size / 2, ivec2(1));
            }
        }
        outTexture.MipMapsArray = std::move(faces);
        outTexture.GPU_Texture2D.RequestReupload = true;
        return true;
    }

	bool SaveTextureToDDS(std::string_view filePath, const Tex& inTexture)
	{
        if (inTexture.MipMapsArray.empty() || inTexture.MipMapsArray[0].empty())
            return false;
        const auto& mips = inTexture.MipMapsArray[0];
        const auto format = mips[0].Format;
        const char* code = nullptr;
        switch (format)
        {
        case TextureFormat::DXT1: case TextureFormat::DXT1a: code = "DXT1"; break;
        case TextureFormat::DXT3: code = "DXT3"; break;
        case TextureFormat::DXT5: code = "DXT5"; break;
        case TextureFormat::RGTC1: code = "ATI1"; break;
        case TextureFormat::RGTC2: code = "ATI2"; break;
        case TextureFormat::RGBA8: break;
        default: return false;
        }
        auto stream = IO::File::CreateWrite(filePath);
        if (!stream.IsOpen())
            return false;
        u32 header[31] = {};
        header[0] = 124;
        header[1] = 0x1007 | (code ? 0x80000 : 8) | (mips.size() > 1 ? 0x20000 : 0);
        header[2] = mips[0].Size.y;
        header[3] = mips[0].Size.x;
        header[4] = code ? mips[0].DataSize : mips[0].Size.x * 4;
        header[6] = mips.size();
        header[18] = 32;
        header[19] = code ? 4 : 0x41;
        if (code)
            std::memcpy(&header[20], code, 4);
        else
        {
            header[21] = 32;
            header[22] = 0xff;
            header[23] = 0xff00;
            header[24] = 0xff0000;
            header[25] = 0xff000000;
        }
        header[26] = 0x1000 | (mips.size() > 1 ? 0x400008 : 0);
        if (inTexture.MipMapsArray.size() == 6)
        {
            header[26] |= 8;
            header[27] = 0xfe00;
        }
        if (stream.WriteBuffer("DDS ", 4) != 4 || stream.WriteBuffer(header, sizeof(header)) != sizeof(header))
            return false;
        for (const auto& face : inTexture.MipMapsArray)
            for (const auto& mip : face)
                if (!mip.Data || mip.Format != format || stream.WriteBuffer(mip.Data.get(), mip.DataSize) != mip.DataSize)
                    return false;
        return true;
    }
}
