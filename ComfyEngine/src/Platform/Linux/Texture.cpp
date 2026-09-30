#include "OpenGL.h"
#include "Graphics/Utilities/TextureCompression.h"
#include "ImGui/ComfyTextureID.h"
namespace Comfy::Render
{
    struct OpenGLTexture final : Graphics::OpaqueGPUResource
    {
        GLuint Name = 0;
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
            const auto rgba = Graphics::Utilities::ConvertTextureToRGBA(texture);
            if (!rgba)
                return 0;
            auto resource = std::make_unique<OpenGLTexture>();
            glGenTextures(1, &resource->Name);
            glBindTexture(GL_TEXTURE_2D, resource->Name);
            const auto size = texture.GetSize();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.get());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            cached.Resource = std::move(resource);
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
