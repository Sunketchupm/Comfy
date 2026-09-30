#pragma once
#include "Types.h"
#include "Graphics/TexSet.h"
#include <GL/glew.h>
namespace Comfy::Render
{
    GLuint GetOpenGLTexture(const Graphics::Tex& texture);
}
namespace Comfy::Platform
{
    void ProcessSDLEvent(const union SDL_Event& event);
    int TranslateSDLKey(int key);
}
