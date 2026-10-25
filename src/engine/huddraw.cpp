// Untextured HUD shapes for game code, which has no access to the engine's shaders.

#include "engine.h"

void hudfillrect(float x0, float y0, float x1, float y1, int colour, int alpha)
{
    hudnotextureshader->set();
    gle::colorub((colour>>16)&0xFF, (colour>>8)&0xFF, colour&0xFF, alpha);
    gle::defvertex(2);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(x0, y0); gle::attribf(x1, y0); gle::attribf(x0, y1); gle::attribf(x1, y1);
    gle::end();
    gle::colorf(1, 1, 1);
    hudshader->set();
}

void hudoutlinerect(float x0, float y0, float x1, float y1, float width, int colour, int alpha)
{
    hudfillrect(x0, y0, x1, y0 + width, colour, alpha);
    hudfillrect(x0, y1 - width, x1, y1, colour, alpha);
    hudfillrect(x0, y0 + width, x0 + width, y1 - width, colour, alpha);
    hudfillrect(x1 - width, y0 + width, x1, y1 - width, colour, alpha);
}
