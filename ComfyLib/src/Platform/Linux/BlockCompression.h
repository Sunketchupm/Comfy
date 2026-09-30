#pragma once

namespace Comfy::Graphics::Utilities
{
    inline void DecodeAlphaBlock(const u8* block, u8* values)
    {
        u8 palette[8] = { block[0], block[1] };
        if (palette[0] > palette[1])
            for (int index = 2; index < 8; index++)
                palette[index] = ((8 - index) * palette[0] + (index - 1) * palette[1]) / 7;
        else
        {
            for (int index = 2; index < 6; index++)
                palette[index] = ((6 - index) * palette[0] + (index - 1) * palette[1]) / 5;
            palette[6] = 0;
            palette[7] = 255;
        }
        u64 indices = 0;
        for (int byte = 0; byte < 6; byte++)
            indices |= u64(block[byte + 2]) << (byte * 8);
        for (int pixel = 0; pixel < 16; pixel++)
            values[pixel] = palette[(indices >> (pixel * 3)) & 7];
    }
    inline void EncodeColorBlock(const u8* pixels, u8* output, bool alpha)
    {
        if (alpha)
        {
            u8 maximum = 0, minimum = 255;
            for (int pixel = 0; pixel < 16; pixel++)
            {
                maximum = Max(maximum, pixels[pixel * 4 + 3]);
                minimum = Min(minimum, pixels[pixel * 4 + 3]);
            }
            output[0] = maximum;
            output[1] = minimum;
            std::fill(output + 2, output + 8, 0);
            u8 palette[16];
            // Decode a block containing each palette index once to obtain the palette.
            u8 probe[8] = {maximum, minimum};
            u64 indices = 0;
            for (int index = 0; index < 8; index++)
                indices |= u64(index) << (index * 3);
            for (int byte = 0; byte < 6; byte++)
                probe[byte + 2] = indices >> (byte * 8);
            DecodeAlphaBlock(probe, palette);
            indices = 0;
            for (int pixel = 0; pixel < 16; pixel++)
            {
                int best = 0, error = 256;
                for (int index = 0; index < 8; index++)
                {
                    const int difference = std::abs(int(pixels[pixel * 4 + 3]) - palette[index]);
                    if (difference < error)
                    {
                        error = difference;
                        best = index;
                    }
                }
                indices |= u64(best) << (pixel * 3);
            }
            for (int byte = 0; byte < 6; byte++)
                output[byte + 2] = indices >> (byte * 8);
            output += 8;
        }
        u8 minimum[3] = {255, 255, 255}, maximum[3] = {};
        for (int pixel = 0; pixel < 16; pixel++)
            for (int channel = 0; channel < 3; channel++)
            {
                minimum[channel] = Min(minimum[channel], pixels[pixel * 4 + channel]);
                maximum[channel] = Max(maximum[channel], pixels[pixel * 4 + channel]);
            }
        auto pack = [](const u8* rgb) -> u16
        {
            return (u16(rgb[0] >> 3) << 11) | (u16(rgb[1] >> 2) << 5) | (rgb[2] >> 3);
        };
        u16 first = pack(maximum), second = pack(minimum);
        if (first == second)
        {
            if (first < 65535)
                first++;
            else
                second--;
        }
        output[0] = first;
        output[1] = first >> 8;
        output[2] = second;
        output[3] = second >> 8;
        u8 palette[4][3];
        for (int index = 0; index < 2; index++)
        {
            const u16 value = index == 0 ? first : second;
            palette[index][0] = ((value >> 11) & 31) * 255 / 31;
            palette[index][1] = ((value >> 5) & 63) * 255 / 63;
            palette[index][2] = (value & 31) * 255 / 31;
        }
        for (int channel = 0; channel < 3; channel++)
        {
            palette[2][channel] = (2 * palette[0][channel] + palette[1][channel]) / 3;
            palette[3][channel] = (palette[0][channel] + 2 * palette[1][channel]) / 3;
        }
        u32 indices = 0;
        for (int pixel = 0; pixel < 16; pixel++)
        {
            int best = 0, error = std::numeric_limits<int>::max();
            for (int index = 0; index < 4; index++)
            {
                int distance = 0;
                for (int channel = 0; channel < 3; channel++)
                {
                    const int difference = int(pixels[pixel * 4 + channel]) - palette[index][channel];
                    distance += difference * difference;
                }
                if (distance < error)
                {
                    error = distance;
                    best = index;
                }
            }
            indices |= u32(best) << (pixel * 2);
        }
        std::memcpy(output + 4, &indices, sizeof(indices));
    }
    inline bool DecodeBlocks(ivec2 size, const u8* input, TextureFormat format, u8* output)
    {
        const size_t blockSize = TextureFormatBlockSize(format);
        for (int y = 0; y < size.y; y += 4)
        {
            for (int x = 0; x < size.x; x += 4, input += blockSize)
            {
                u8 pixels[16][4] = {};
                if (format == TextureFormat::RGTC1 || format == TextureFormat::RGTC2)
                {
                    u8 red[16], green[16] = {};
                    DecodeAlphaBlock(input, red);
                    if (format == TextureFormat::RGTC2)
                        DecodeAlphaBlock(input + 8, green);
                    for (int pixel = 0; pixel < 16; pixel++)
                    {
                        pixels[pixel][0] = red[pixel];
                        pixels[pixel][1] = green[pixel];
                        pixels[pixel][3] = 255;
                    }
                }
                else
                {
                    const u8* color = input + (blockSize == 16 ? 8 : 0);
                    const u16 first = color[0] | (u16(color[1]) << 8);
                    const u16 second = color[2] | (u16(color[3]) << 8);
                    u8 palette[4][4] = {};
                    for (int index = 0; index < 2; index++)
                    {
                        const u16 packed = index == 0 ? first : second;
                        palette[index][0] = ((packed >> 11) & 31) * 255 / 31;
                        palette[index][1] = ((packed >> 5) & 63) * 255 / 63;
                        palette[index][2] = (packed & 31) * 255 / 31;
                        palette[index][3] = 255;
                    }
                    for (int channel = 0; channel < 3; channel++)
                    {
                        if (first > second || blockSize == 16)
                        {
                            palette[2][channel] = (2 * palette[0][channel] + palette[1][channel]) / 3;
                            palette[3][channel] = (palette[0][channel] + 2 * palette[1][channel]) / 3;
                        }
                        else
                            palette[2][channel] = (palette[0][channel] + palette[1][channel]) / 2;
                    }
                    palette[2][3] = 255;
                    palette[3][3] = (first > second || blockSize == 16) ? 255 : 0;
                    u32 indices = 0;
                    std::memcpy(&indices, color + 4, 4);
                    u8 alpha[16];
                    if (format == TextureFormat::DXT5)
                        DecodeAlphaBlock(input, alpha);
                    for (int pixel = 0; pixel < 16; pixel++)
                    {
                        std::memcpy(pixels[pixel], palette[(indices >> (pixel * 2)) & 3], 4);
                        if (format == TextureFormat::DXT3)
                            pixels[pixel][3] = ((input[pixel / 2] >> ((pixel % 2) * 4)) & 15) * 17;
                        if (format == TextureFormat::DXT5)
                            pixels[pixel][3] = alpha[pixel];
                    }
                }
                for (int row = 0; row < 4 && y + row < size.y; row++)
                    for (int column = 0; column < 4 && x + column < size.x; column++)
                        std::memcpy(output + ((y + row) * size.x + x + column) * 4, pixels[row * 4 + column], 4);
            }
        }
        return true;
    }
}
