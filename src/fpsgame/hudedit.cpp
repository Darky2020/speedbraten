// HUD editor: /hudedit lets the player move and scale HUD elements with the mouse. Each element
// reports the area it drew with hudeditarea under the prefix of its settings; the editor changes
// <prefix>x, <prefix>y (the element's anchor as a fraction of the screen) and <prefix>scale.

#include "game.h"
#include "sbcompat.h"

extern int screenw, screenh;
extern float guisens;

namespace game
{
    bool hudediting = false;

    struct hudarea
    {
        const char *name;
        float x0, y0, x1, y1;
    };
    // the areas reported while the current frame draws, and those of the last drawn frame
    static vector<hudarea> drawing, drawn;
    static int hudw = 0, hudh = 0;
    static float cursorx = 0.5f, cursory = 0.5f; // fraction of the HUD
    static const char *grabbed = NULL;
    static bool scaling = false;
    static float grabdx = 0, grabdy = 0;

    struct hudstart { const char *name; float x, y, scale; };
    static vector<hudstart> starts; // each element as it was when the editor opened

    static ident *hudvar(const char *name, const char *field)
    {
        defformatstring(var, "%s%s", name, field);
        ident *id = getident(var);
        return id && id->type == ID_FVAR ? id : NULL;
    }

    static float hudget(const char *name, const char *field)
    {
        ident *id = hudvar(name, field);
        return id ? *id->storage.f : 0;
    }

    static void hudset(const char *name, const char *field, float val)
    {
        ident *id = hudvar(name, field);
        if(id) setfvarchecked(id, val);
    }

    void hudeditarea(const char *name, float x0, float y0, float x1, float y1)
    {
        if(!hudediting) return;
        loopv(drawing) if(!strcmp(drawing[i].name, name))
        {
            hudarea &a = drawing[i];
            a.x0 = min(a.x0, x0); a.y0 = min(a.y0, y0);
            a.x1 = max(a.x1, x1); a.y1 = max(a.y1, y1);
            return;
        }
        if(!hudvar(name, "x") || !hudvar(name, "y")) return;
        hudarea &a = drawing.add();
        a.name = name;
        a.x0 = x0; a.y0 = y0; a.x1 = x1; a.y1 = y1;
        loopv(starts) if(!strcmp(starts[i].name, name)) return;
        hudstart &s = starts.add();
        s.name = name;
        s.x = hudget(name, "x"); s.y = hudget(name, "y"); s.scale = hudget(name, "scale");
    }

    static const char *hudhover()
    {
        if(grabbed) return grabbed;
        const float pad = 8;
        float cx = cursorx*hudw, cy = cursory*hudh;
        for(int i = drawn.length()-1; i >= 0; i--)
        {
            const hudarea &a = drawn[i];
            if(cx >= a.x0 - pad && cx <= a.x1 + pad && cy >= a.y0 - pad && cy <= a.y1 + pad) return a.name;
        }
        return NULL;
    }

    static const char *hudeditblocked()
    {
        bool running = false;
        runtimerticks(running);
        if(!connected) return "the HUD editor works in a game";
        if(running) return "the HUD editor does not work during a run";
        if(inputlocked()) return "the HUD editor does not work during a replay";
        return NULL;
    }

    static void closehudedit()
    {
        hudediting = false;
        grabbed = NULL;
        scaling = false;
        drawing.setsize(0);
        drawn.setsize(0);
        starts.setsize(0);
    }

    ICOMMAND(hudedit, "", (),
    {
        if(hudediting) { closehudedit(); return; }
        if(const char *why = hudeditblocked()) { conoutf(CON_ERROR, "%s", why); return; }
        hudediting = true;
        cursorx = cursory = 0.5f;
    });

    bool hudeditmove(int dx, int dy)
    {
        if(!hudediting) return false;
        const float cursorscale = 500.0f;
        float oldy = cursory;
        cursorx = clamp(cursorx + guisens*dx*(screenh/(screenw*cursorscale)), 0.0f, 1.0f);
        cursory = clamp(cursory + guisens*dy/cursorscale, 0.0f, 1.0f);
        if(grabbed && scaling) hudset(grabbed, "scale", hudget(grabbed, "scale")*(1 - (cursory - oldy)*2));
        else if(grabbed)
        {
            hudset(grabbed, "x", cursorx - grabdx);
            hudset(grabbed, "y", cursory - grabdy);
        }
        return true;
    }

    bool hudeditkey(int code, bool isdown)
    {
        if(!hudediting) return false;
        switch(code)
        {
            case SDLK_ESCAPE:
                if(isdown) closehudedit();
                return true;
            case SDLK_r:
                if(isdown) if(const char *name = hudhover()) loopv(starts) if(!strcmp(starts[i].name, name))
                {
                    hudset(name, "x", starts[i].x);
                    hudset(name, "y", starts[i].y);
                    hudset(name, "scale", starts[i].scale);
                }
                return true;
            case -1: case -3:
                if(!isdown) { grabbed = NULL; scaling = false; }
                else if(const char *name = hudhover())
                {
                    grabbed = name;
                    scaling = code == -3;
                    grabdx = cursorx - hudget(name, "x");
                    grabdy = cursory - hudget(name, "y");
                }
                return true;
            case -4: case -5:
                if(isdown) if(const char *name = hudhover()) hudset(name, "scale", hudget(name, "scale")*(code == -4 ? 1.08f : 1/1.08f));
                return true;
        }
        return code < 0;
    }

    void drawhudedit(int w, int h)
    {
        if(!hudediting) return;
        if(const char *why = hudeditblocked())
        {
            conoutf("%s, closed", why);
            closehudedit();
            return;
        }
        hudw = w;
        hudh = h;
        drawn.setsize(0);
        drawn.put(drawing.getbuf(), drawing.length());
        drawing.setsize(0);

        const char *hover = hudhover();
        loopv(drawn)
        {
            const hudarea &a = drawn[i];
            bool on = hover && !strcmp(a.name, hover);
            float x0 = a.x0 - 6, y0 = a.y0 - 4, x1 = a.x1 + 6, y1 = a.y1 + 4;
            hudfillrect(x0, y0, x1, y1, on ? 0xFFC040 : 0x40A0FF, on ? 60 : 30);
            hudoutlinerect(x0, y0, x1, y1, 2, on ? 0xFFC040 : 0x40A0FF, on ? 230 : 140);
        }

        const float scale = 0.4f;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        loopv(drawn)
        {
            const hudarea &a = drawn[i];
            int tw = 0, th = 0;
            text_bounds(a.name, tw, th);
            draw_text(a.name, int((a.x0 - 6)/scale), int((a.y0 - 6)/scale) - th, 255, 255, 255, 200);
        }
        const char *help = "\f7drag \f2move\f7   right drag or wheel \f2scale\f7   R \f2undo\f7   Esc \f2done";
        int tw = 0, th = 0;
        text_bounds(help, tw, th);
        draw_text(help, int(w/2.0f/scale - tw/2.0f), int(h*0.86f/scale), 255, 255, 255, 220);
        pophudmatrix();

        float cx = cursorx*w, cy = cursory*h, r = max(h/120.0f, 4.0f);
        hudfillrect(cx - r, cy - 1.5f, cx + r, cy + 1.5f, 0xFFFFFF);
        hudfillrect(cx - 1.5f, cy - r, cx + 1.5f, cy + r, 0xFFFFFF);
    }
}
