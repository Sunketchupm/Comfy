#include "OpenGL.h"
#include <stdexcept>
#include <cstddef>
#include <vector>
#include "Render/Core/Renderer2D/Renderer2D.h"
#include "Render/Core/Renderer2D/Detail/SpriteBatchData.h"
namespace Comfy::Render
{
    class OpenGLRenderTarget final : public RenderTarget2D
    {
    public:
        GLuint Texture = 0, Framebuffer = 0, ProcessedTexture = 0, ProcessedFramebuffer = 0;
        ivec2 Size = {};
        ~OpenGLRenderTarget() override
        {
            glDeleteFramebuffers(1, &Framebuffer);
            glDeleteTextures(1, &Texture);
            glDeleteTextures(1, &ProcessedTexture);
            glDeleteFramebuffers(1, &ProcessedFramebuffer);
        }
        void Resize()
        {
            const ivec2 desired = Max(Param.Resolution, ivec2(1));
            if (desired == Size)
                return;
            Size = desired;
            if (!Texture)
                glGenTextures(1, &Texture);
            if (!Framebuffer)
                glGenFramebuffers(1, &Framebuffer);
            glBindTexture(GL_TEXTURE_2D, Texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Size.x, Size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, Framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Texture, 0);
            if (!ProcessedTexture)
                glGenTextures(1, &ProcessedTexture);
            if (!ProcessedFramebuffer)
                glGenFramebuffers(1, &ProcessedFramebuffer);
            glBindTexture(GL_TEXTURE_2D, ProcessedTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Size.x, Size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, ProcessedFramebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ProcessedTexture, 0);
        }
        ComfyTextureID GetTextureID() const override
        {
            ComfyTextureID id;
            id.Data.ResourceView = Param.PostProcessingEnabled ? ProcessedTexture : Texture;
            return id;
        }
        std::unique_ptr<u8[]> TakeScreenshot() override
        {
            auto result = std::make_unique<u8[]>(size_t(Size.x) * Size.y * 4);
            GLint previous;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
            glBindFramebuffer(GL_FRAMEBUFFER, Param.PostProcessingEnabled ? ProcessedFramebuffer : Framebuffer);
            glReadPixels(0, 0, Size.x, Size.y, GL_RGBA, GL_UNSIGNED_BYTE, result.get());
            glBindFramebuffer(GL_FRAMEBUFFER, previous);
            return result;
        }
    };
    namespace
    {
        constexpr size_t MaxBatchVertices = 6144;

        GLuint CreateSpriteProgram(const char* fragmentSource)
        {
            const char* vertexSource = "#version 120\n"
                "varying vec2 uv; varying vec2 maskUV; varying vec4 color;\n"
                "void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;"
                "uv = gl_MultiTexCoord0.xy; maskUV = gl_MultiTexCoord1.xy; color = gl_Color; }";
            auto compile = [](GLenum type, const char* source)
            {
                const GLuint shader = glCreateShader(type);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint success = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
                if (!success)
                {
                    char message[1024];
                    glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
                    glDeleteShader(shader);
                    throw std::runtime_error(message);
                }
                return shader;
            };
            const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
            const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint success = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &success);
            if (!success)
                throw std::runtime_error("Could not link the sprite shader");
            return program;
        }
        void EmitSpriteVertex(const Detail::SpriteVertex& vertex, const vec4& color)
        {
            glColor4fv(glm::value_ptr(color));
            glMultiTexCoord2fv(GL_TEXTURE0, glm::value_ptr(vertex.TextureCoordinates));
            glMultiTexCoord2fv(GL_TEXTURE1, glm::value_ptr(vertex.TextureMaskCoordinates));
            glVertex2fv(glm::value_ptr(vertex.Position));
        }
    }
    struct Renderer2D::Impl
    {
        Render::AetRenderer Aet;
        Render::FontRenderer Font;
        Camera2D* Camera = nullptr;
        OpenGLRenderTarget* Target = nullptr;
        GLint PreviousFramebuffer = 0, PreviousProgram = 0;
        GLuint MaskProgram = 0, CheckerboardProgram = 0, PostProcessProgram = 0;
        GLuint VertexBuffer = 0;
        std::vector<PositionTextureColorVertex> Vertices;
        TexSamplerView BatchView;
        Graphics::AetBlendMode BatchBlend = {};
        Graphics::PrimitiveType BatchPrimitive = {};
        void Flush();
        Impl(Renderer2D& renderer) : Aet(renderer), Font(renderer)
        {
            glGenBuffers(1, &VertexBuffer);
            Vertices.reserve(MaxBatchVertices);
            MaskProgram = CreateSpriteProgram("#version 120\n"
                "uniform sampler2D sprite; uniform sampler2D mask; uniform int textured;"
                "varying vec2 uv; varying vec2 maskUV; varying vec4 color;"
                "void main() { vec4 pixel = textured != 0 ? texture2D(sprite, vec2(maskUV.x, 1.0 - maskUV.y)) : vec4(1.0);"
                "gl_FragColor = pixel * color; gl_FragColor.a *= texture2D(mask, vec2(uv.x, 1.0 - uv.y)).a; }");
            PostProcessProgram = CreateSpriteProgram("#version 120\n"
                "uniform sampler2D image; uniform float gamma; uniform float contrast;"
                "uniform vec3 coefficientR; uniform vec3 coefficientG; uniform vec3 coefficientB;"
                "varying vec2 uv;"
                "void main() { vec4 pixel = texture2D(image, uv);"
                "vec3 corrected = pow(max(abs(pixel.rgb), vec3(0.0000000001)), vec3(gamma));"
                "vec3 mixed = clamp(vec3(dot(corrected, coefficientR), dot(corrected, coefficientG),"
                "dot(corrected, coefficientB)), 0.0, 1.0);"
                "gl_FragColor = vec4(pow(max(abs(mixed), vec3(0.0000000001)), vec3(contrast)), pixel.a); }");
            CheckerboardProgram = CreateSpriteProgram("#version 120\n"
                "uniform vec2 cells; varying vec2 uv; varying vec4 color;"
                "void main() { if (mod(floor(uv.x * cells.x) + floor(uv.y * cells.y), 2.0) < 1.0) discard;"
                "gl_FragColor = color; }");
        }
        ~Impl()
        {
            glDeleteBuffers(1, &VertexBuffer);
            glDeleteProgram(MaskProgram);
            glDeleteProgram(CheckerboardProgram);
            glDeleteProgram(PostProcessProgram);
        }
    };
    Renderer2D::Renderer2D() : impl(std::make_unique<Impl>(*this)) {}
    Renderer2D::~Renderer2D() = default;
    void Renderer2D::Begin(Camera2D& camera, RenderTarget2D& target)
    {
        impl->Camera = &camera;
        impl->Target = static_cast<OpenGLRenderTarget*>(&target);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &impl->PreviousFramebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &impl->PreviousProgram);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        impl->Target->Resize();
        glBindFramebuffer(GL_FRAMEBUFFER, impl->Target->Framebuffer);
        glViewport(0, 0, impl->Target->Size.x, impl->Target->Size.y);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        if (target.Param.Clear)
        {
            const auto color = target.Param.ClearColor;
            glClearColor(color.r, color.g, color.b, color.a);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        // The target is sampled with top-left UV coordinates by ImGui.
        glOrtho(0, camera.ProjectionSize.x, 0, camera.ProjectionSize.y, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glTranslatef(-camera.Position.x, -camera.Position.y, 0);
        glScalef(camera.Zoom, camera.Zoom, 1);
        glEnable(GL_BLEND);
    }
    void Renderer2D::DrawVertices(const PositionTextureColorVertex* vertices, size_t count,
                                  TexSamplerView view, Graphics::AetBlendMode blend, Graphics::PrimitiveType primitive)
    {
        if (!vertices || !count)
            return;
        // Do not join strips/fans or incomplete primitives across draw boundaries.
        size_t groupSize = 0;
        switch (primitive)
        {
        case Graphics::PrimitiveType::Points: groupSize = 1; break;
        case Graphics::PrimitiveType::Lines: groupSize = 2; break;
        case Graphics::PrimitiveType::Triangles: groupSize = 3; break;
        case Graphics::PrimitiveType::Quads: groupSize = 4; break;
        default: break;
        }
        const bool canMerge = groupSize != 0 && count % groupSize == 0;
        if (!canMerge || impl->BatchView != view || impl->BatchView.Filter != view.Filter
            || impl->BatchBlend != blend || impl->BatchPrimitive != primitive
            || impl->Vertices.size() + count > MaxBatchVertices)
            impl->Flush();
        impl->BatchView = view;
        impl->BatchBlend = blend;
        impl->BatchPrimitive = primitive;
        for (size_t index = 0; index < count; index++)
        {
            auto vertex = vertices[index];
            vertex.TextureCoordinates.y = 1.0f - vertex.TextureCoordinates.y;
            impl->Vertices.push_back(vertex);
        }
        if (!canMerge)
            impl->Flush();
    }
    void Renderer2D::Impl::Flush()
    {
        if (Vertices.empty())
            return;
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        switch (BatchBlend)
        {
        case Graphics::AetBlendMode::Add: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
        case Graphics::AetBlendMode::LinearDodge: glBlendFunc(GL_ONE, GL_ONE); break;
        case Graphics::AetBlendMode::Multiply: glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
        default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        }
        const auto& view = BatchView;
        if (view)
        {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, GetOpenGLTexture(*view.Texture));
            const GLint filter = view.Filter == Graphics::TextureFilter::Point ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            auto address = [](Graphics::TextureAddressMode mode)
            {
                if (mode == Graphics::TextureAddressMode::WrapRepeat)
                    return GL_REPEAT;
                if (mode == Graphics::TextureAddressMode::WrapRepeatMirror)
                    return GL_MIRRORED_REPEAT;
                return mode == Graphics::TextureAddressMode::ClampBorder ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
            };
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, address(view.AddressU));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, address(view.AddressV));
        }
        else
            glDisable(GL_TEXTURE_2D);
        // The placement grid supplies independent line segments. Preserve the
        // requested topology instead of interpreting every vertex list as triangles.
        GLenum mode;
        switch (BatchPrimitive)
        {
        case Graphics::PrimitiveType::Points: mode = GL_POINTS; break;
        case Graphics::PrimitiveType::Lines: mode = GL_LINES; break;
        case Graphics::PrimitiveType::LineStrip: mode = GL_LINE_STRIP; break;
        case Graphics::PrimitiveType::LineLoop: mode = GL_LINE_LOOP; break;
        case Graphics::PrimitiveType::Triangles: mode = GL_TRIANGLES; break;
        case Graphics::PrimitiveType::TriangleStrip: mode = GL_TRIANGLE_STRIP; break;
        case Graphics::PrimitiveType::TriangleFan: mode = GL_TRIANGLE_FAN; break;
        case Graphics::PrimitiveType::Quads: mode = GL_QUADS; break;
        case Graphics::PrimitiveType::QuadStrip: mode = GL_QUAD_STRIP; break;
        case Graphics::PrimitiveType::Polygon: mode = GL_POLYGON; break;
        default: throw std::invalid_argument("Unsupported 2D primitive type");
        }
        GLint previousBuffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
        glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
        glClientActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
        // Replace storage so queued draws can keep using the previous upload.
        glBufferData(GL_ARRAY_BUFFER, Vertices.size() * sizeof(PositionTextureColorVertex),
                     Vertices.data(), GL_STREAM_DRAW);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(2, GL_FLOAT, sizeof(PositionTextureColorVertex),
                        reinterpret_cast<void*>(offsetof(PositionTextureColorVertex, Position)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(PositionTextureColorVertex),
                          reinterpret_cast<void*>(offsetof(PositionTextureColorVertex, TextureCoordinates)));
        glColorPointer(4, GL_FLOAT, sizeof(PositionTextureColorVertex),
                       reinterpret_cast<void*>(offsetof(PositionTextureColorVertex, Color)));
        glDrawArrays(mode, 0, static_cast<GLsizei>(Vertices.size()));
        glPopClientAttrib();
        glBindBuffer(GL_ARRAY_BUFFER, previousBuffer);
        Vertices.clear();
    }
    void Renderer2D::Draw(const RenderCommand2D& command)
    {
        if (command.DrawTextBorder && command.TexView)
        {
            auto border = command;
            border.DrawTextBorder = false;
            for (auto& color : border.CornerColors)
                color = {0, 0, 0, color.a};
            const float offset = 1.0f / impl->Camera->Zoom;
            for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++)
                {
                    if (x == 0 && y == 0)
                        continue;
                    border.Position = command.Position + vec2(x, y) * offset;
                    Draw(border);
                }
        }
        Detail::SpriteQuadVertices quad;
        const vec2 textureSize = command.TexView ? vec2(command.TexView.Texture->GetSize()) : vec2(1);
        quad.SetValues(command.Position, command.SourceRegion, textureSize, -command.Origin,
                       command.Rotation, command.Scale, command.CornerColors.data(), bool(command.TexView),
                       command.TexView && command.TexView.Texture->GPU_FlipY);
        const Detail::SpriteVertex* corners[] = { &quad.TopLeft, &quad.BottomLeft, &quad.BottomRight,
                                                  &quad.BottomRight, &quad.TopRight, &quad.TopLeft };
        const int colors[] = {0, 2, 3, 3, 1, 0};
        PositionTextureColorVertex vertices[6];
        for (int index = 0; index < 6; index++)
            vertices[index] = {corners[index]->Position, corners[index]->TextureCoordinates, command.CornerColors[colors[index]]};
        DrawVertices(vertices, 6, command.TexView, command.BlendMode);
    }
    void Renderer2D::Draw(const RenderCommand2D& command, const RenderCommand2D& mask)
    {
        if (!mask.TexView)
        {
            Draw(command);
            return;
        }
        impl->Flush();
        Detail::SpriteQuadVertices quad;
        const vec2 size = vec2(mask.TexView.Texture->GetSize());
        quad.SetValues(mask.Position, mask.SourceRegion, size, -mask.Origin,
                       mask.Rotation, mask.Scale, mask.CornerColors.data(), true, mask.TexView.Texture->GPU_FlipY);
        quad.SetTexMaskCoords(command.TexView, command.Position, command.Scale,
                              command.Origin + vec2(command.SourceRegion.x, command.SourceRegion.y), command.Rotation,
                              mask.Position, mask.Scale, mask.Origin, mask.Rotation, mask.SourceRegion);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, GetOpenGLTexture(*mask.TexView.Texture));
        glActiveTexture(GL_TEXTURE0);
        if (command.TexView)
        {
            glBindTexture(GL_TEXTURE_2D, GetOpenGLTexture(*command.TexView.Texture));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        }
        glUseProgram(impl->MaskProgram);
        glUniform1i(glGetUniformLocation(impl->MaskProgram, "sprite"), 0);
        glUniform1i(glGetUniformLocation(impl->MaskProgram, "mask"), 1);
        glUniform1i(glGetUniformLocation(impl->MaskProgram, "textured"), bool(command.TexView));
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        EmitSpriteVertex(quad.TopLeft, command.CornerColors[0]);
        EmitSpriteVertex(quad.TopRight, command.CornerColors[1]);
        EmitSpriteVertex(quad.BottomRight, command.CornerColors[3]);
        EmitSpriteVertex(quad.BottomLeft, command.CornerColors[2]);
        glEnd();
        glUseProgram(0);
    }
    void Renderer2D::DrawLine(vec2 start, vec2 end, const vec4& color, float thickness)
    {
        const vec2 edge = end - start;
        DrawLine(start, glm::degrees(glm::atan(edge.y, edge.x)), glm::length(edge), color, thickness);
    }
    void Renderer2D::DrawLine(vec2 start, float angle, float length, const vec4& color, float thickness)
    {
        RenderCommand2D command;
        command.Position = start;
        command.Origin = {0, thickness / 2};
        command.Rotation = angle;
        command.SourceRegion = {0, 0, length, thickness};
        command.SetColor(color);
        Draw(command);
    }
    void Renderer2D::DrawRect(vec2 tl, vec2 tr, vec2 bl, vec2 br, const vec4& color, float thickness)
    {
        DrawLine(tl, tr, color, thickness);
        DrawLine(tr, br, color, thickness);
        DrawLine(br, bl, color, thickness);
        DrawLine(bl, tl, color, thickness);
    }
    void Renderer2D::DrawRectCheckerboard(vec2 position, vec2 size, vec2 origin, float rotation, vec2 scale,
                                        const vec4& color, float precision)
    {
        Detail::SpriteQuadVertices quad;
        impl->Flush();
        const vec4 colors[] = {color, color, color, color};
        quad.SetValues(position, vec4(0, 0, size), size, -origin, rotation, scale, colors, true, false);
        glUseProgram(impl->CheckerboardProgram);
        const vec2 cells = size * scale * precision;
        glUniform2f(glGetUniformLocation(impl->CheckerboardProgram, "cells"), cells.x, cells.y);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        EmitSpriteVertex(quad.TopLeft, color);
        EmitSpriteVertex(quad.TopRight, color);
        EmitSpriteVertex(quad.BottomRight, color);
        EmitSpriteVertex(quad.BottomLeft, color);
        glEnd();
        glUseProgram(0);
    }
    void Renderer2D::End()
    {
        impl->Flush();
        if (impl->Target->Param.PostProcessingEnabled)
        {
            const auto& settings = impl->Target->Param.PostProcessing;
            glBindFramebuffer(GL_FRAMEBUFFER, impl->Target->ProcessedFramebuffer);
            glDisable(GL_BLEND);
            glUseProgram(impl->PostProcessProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, impl->Target->Texture);
            glUniform1i(glGetUniformLocation(impl->PostProcessProgram, "image"), 0);
            glUniform1f(glGetUniformLocation(impl->PostProcessProgram, "gamma"), settings.Gamma);
            glUniform1f(glGetUniformLocation(impl->PostProcessProgram, "contrast"), settings.Contrast);
            const char* names[] = {"coefficientR", "coefficientG", "coefficientB"};
            for (int index = 0; index < 3; index++)
                glUniform3fv(glGetUniformLocation(impl->PostProcessProgram, names[index]), 1,
                             glm::value_ptr(settings.ColorCoefficientsRGB[index]));
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, 1, 0, 1, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glBegin(GL_QUADS);
            glTexCoord2f(0,0); glVertex2f(0,0);
            glTexCoord2f(1,0); glVertex2f(1,0);
            glTexCoord2f(1,1); glVertex2f(1,1);
            glTexCoord2f(0,1); glVertex2f(0,1);
            glEnd();
        }
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glPopAttrib();
        glBindFramebuffer(GL_FRAMEBUFFER, impl->PreviousFramebuffer);
        glUseProgram(impl->PreviousProgram);
        impl->Camera = nullptr;
        impl->Target = nullptr;
    }
    AetRenderer& Renderer2D::Aet() { return impl->Aet; }
    FontRenderer& Renderer2D::Font() { return impl->Font; }
    const Camera2D& Renderer2D::GetCamera() const { return *impl->Camera; }
    RenderTarget2D& Renderer2D::GetRenderTarget() const { return *impl->Target; }
    std::unique_ptr<RenderTarget2D> Renderer2D::CreateRenderTarget() { return std::make_unique<OpenGLRenderTarget>(); }
    void Renderer2D::UploadToGPUFreeCPUMemory(Graphics::Tex& texture) { GetOpenGLTexture(texture); }
    void Renderer2D::UploadToGPUFreeCPUMemory(Graphics::TexSet& set)
    {
        for (auto& texture : set.Textures)
            if (texture)
                UploadToGPUFreeCPUMemory(*texture);
    }
    void Renderer2D::UploadToGPUFreeCPUMemory(Graphics::SprSet& set) { UploadToGPUFreeCPUMemory(set.TexSet); }
}
