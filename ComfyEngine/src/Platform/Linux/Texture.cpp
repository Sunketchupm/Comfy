#include "OpenGL.h"
#include "Graphics/Utilities/TextureCompression.h"
#include "ImGui/ComfyTextureID.h"
namespace Comfy::Render
{
    struct OpenGLTexture final : Graphics::OpaqueGPUResource
    {
        GLuint Name = 0;
        ivec2 Size = {};
        ~OpenGLTexture() override
        {
            if (Name)
                glDeleteTextures(1, &Name);
        }
    };
    GLuint GetOpenGLTexture(const Graphics::Tex& texture)
    {
        auto& cached = texture.GPU_Texture2D;
        if (!cached.Resource || cached.RequestReupload)
        {
            if (texture.MipMapsArray.empty() || texture.MipMapsArray.front().empty())
                return 0;
            // Video frames already contain RGBA pixels. Upload them directly
            // instead of copying each pixel through the texture decoder.
            const auto& mip = texture.MipMapsArray.front().front();
            std::unique_ptr<u8[]> convertedPixels;
            const u8* pixels;
            if (mip.Format == Graphics::TextureFormat::RGBA8)
                pixels = mip.Data.get();
            else
            {
                convertedPixels = Graphics::Utilities::ConvertTextureToRGBA(texture);
                pixels = convertedPixels.get();
            }
            if (!pixels)
                return 0;
            const auto size = texture.GetSize();
            if (cached.Resource && static_cast<OpenGLTexture*>(cached.Resource.get())->Size == size)
            {
                auto& resource = *static_cast<OpenGLTexture*>(cached.Resource.get());
                glBindTexture(GL_TEXTURE_2D, resource.Name);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            }
            else
            {
                auto resource = std::make_unique<OpenGLTexture>();
                resource->Size = size;
                glGenTextures(1, &resource->Name);
                glBindTexture(GL_TEXTURE_2D, resource->Name);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                cached.Resource = std::move(resource);
            }
            cached.RequestReupload = false;
        }
        return static_cast<OpenGLTexture*>(cached.Resource.get())->Name;
    }
}
namespace Comfy
{
    ComfyTextureID::ComfyTextureID(ID3D11ShaderResourceView* view) { Data.ResourceView = reinterpret_cast<u64>(view); }
    ComfyTextureID::ComfyTextureID(const Graphics::Tex& texture) { Data.ResourceView = Render::GetOpenGLTexture(texture); }
    ComfyTextureID::ComfyTextureID(const Graphics::LightMapIBL&) {}
}
