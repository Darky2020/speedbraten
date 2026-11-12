// Replay input display: while a replay is watched, the movement keys, jump and the mouse buttons
// as boxes labelled with the player's own keys, lit while held, a row with every other bound key
// the replay uses, and the mouse movement of the last 100 ms as a dot in a circle with a trail. Read from the replay's raw input (keys with what they
// were bound to, raw mouse motion); replays without raw input fall back to the recorded controls.
// It only reads the replay, never the game state.

#include "game.h"
#include "sbcompat.h"
#include "inputs.h"

extern const char *getkeyname(int code);

namespace game
{
    MOD(VARP, replayinputs, 0, 1, 1);
    MOD(FVARP, replayinputscale, 0.5, 1.0, 3.0);
    MOD(FVARP, replayinputx, 0.0, 0.12, 1.0);
    MOD(FVARP, replayinputy, 0.0, 0.66, 1.0);

    enum { A_FORWARD = 0, A_BACKWARD, A_LEFT, A_RIGHT, A_JUMP, NUMACTIONS };
    static const char * const actionbinds[NUMACTIONS] = { "forward", "backward", "left", "right", "jump" };
    static const char * const actiondefaults[NUMACTIONS] = { "W", "S", "A", "D", "SPACE" };

    // the mouse movement window, the trail's spacing and length, in ticks
    enum { MOTIONTICKS = 100/SIMTICK, TRAILSTEP = 2, TRAILDOTS = 8, WHEELTICKS = 4 };
    // a tap stays lit this long after its release, in ticks, so that it can be seen
    enum { TAPTICKS = 150/SIMTICK, MAXEXTRAS = 24 };
    // the keymap's codes of the wheel's steps
    enum { WHEELUP = -4, WHEELDOWN = -5 };
    // degrees of aim, without raw input, at which the dot is 63% of the way out; with raw input it is
    // the replay's own 75th percentile of movement per window, as mice and sensitivities differ
    static const float AIMSCALE = 20, MINMOTIONSCALE = 10;

    // whether a bind's text names the action as a word (bind actions can be scripts)
    static bool bindhas(const char *bind, const char *action)
    {
        if(!bind) return false;
        int n = strlen(action);
        for(const char *s = bind; (s = strstr(s, action)); s += n)
        {
            bool before = s == bind || !isalnum(uchar(s[-1])), after = !isalnum(uchar(s[n]));
            if(before && after) return true;
        }
        return false;
    }

    // what a raw event pressed: keys by keycode, mouse buttons and wheel steps as the keymap's
    // negative codes
    static int inputcode(const rawevent &e)
    {
        switch(e.kind)
        {
            case rawevent::BUTTONDOWN: case rawevent::BUTTONUP: return -e.code;
            case rawevent::WHEEL: return e.dy > 0 ? WHEELUP : WHEELDOWN;
            default: return e.keycode;
        }
    }

    // a key that opens the chat or console prompt: its release goes to the prompt and is never recorded
    static bool opensprompt(const char *bind) { return bindhas(bind, "saycommand") || bindhas(bind, "sayteamcommand"); }

    struct heldinput { int code; const char *bind; };
    // a key outside the fixed layout, and when it was last pressed and released
    struct extrakey { int code, down, up; string label; };

    static struct displaystate
    {
        const inputattempt *attempt;
        int cursor, tick;
        vector<heldinput> held;
        int wheeljump; // last tick a wheel step bound to jump moved
        string labels[NUMACTIONS];
        vector<extrakey> extras;
        float motionscale;

        displaystate() : attempt(NULL), cursor(0), tick(-1), wheeljump(-1000), motionscale(AIMSCALE) {}
    } st;

    static bool extraorder(const extrakey &x, const extrakey &y) { return (x.code < 0) != (y.code < 0) ? x.code > y.code : x.code < y.code; }

    static void setlabels(const inputattempt &a)
    {
        loopi(NUMACTIONS)
        {
            copystring(st.labels[i], actiondefaults[i]);
            loopvj(a.raw)
            {
                const rawevent &e = a.raw[j];
                if(e.kind != rawevent::KEYDOWN || !bindhas(e.bind, actionbinds[i])) continue;
                const char *name = getkeyname(inputcode(e));
                if(name) copystring(st.labels[i], name);
                break;
            }
        }

        // every other bound key the replay uses, keys in keycode order, then mouse buttons and wheel; a
        // mouse button is listed whatever it is bound to, as the layout's action boxes are keyboard keys
        st.extras.setsize(0);
        loopv(a.raw)
        {
            const rawevent &e = a.raw[i];
            if((e.kind != rawevent::KEYDOWN && e.kind != rawevent::BUTTONDOWN && e.kind != rawevent::WHEEL) || e.flags&rawevent::TEXT || !e.bind || !e.bind[0]) continue;
            int code = inputcode(e);
            if(code == -1 || code == -3) continue;
            bool known = false;
            if(e.kind != rawevent::BUTTONDOWN) loopj(NUMACTIONS) if(bindhas(e.bind, actionbinds[j])) known = true;
            loopvj(st.extras) if(st.extras[j].code == code) known = true;
            if(known || st.extras.length() >= MAXEXTRAS) continue;
            extrakey &x = st.extras.add();
            x.code = code;
            x.down = x.up = -1000;
            const char *name = getkeyname(code);
            copystring(x.label, name ? name : "?");
        }
        st.extras.sort(extraorder);

        st.motionscale = AIMSCALE;
        vector<float> windows;
        for(int i = 0; i < a.raw.length();)
        {
            int start = a.raw[i].tick;
            float dx = 0, dy = 0;
            for(; i < a.raw.length() && a.raw[i].tick < start + MOTIONTICKS; i++)
                if(a.raw[i].kind == rawevent::MOTION) { dx += a.raw[i].dx; dy += a.raw[i].dy; }
            if(dx || dy) windows.add(sqrtf(dx*dx + dy*dy));
        }
        if(windows.length())
        {
            windows.sort();
            st.motionscale = max(MINMOTIONSCALE, windows[windows.length()*3/4]);
        }
    }

    // the keys held at tick t, from the attempt's raw events up to it
    static void advance(const inputattempt &a, int t)
    {
        if(st.attempt != &a || t < st.tick)
        {
            if(st.attempt != &a) setlabels(a);
            st.attempt = &a;
            st.cursor = 0;
            st.held.setsize(0);
            st.wheeljump = -1000;
            loopv(st.extras) st.extras[i].down = st.extras[i].up = -1000;
        }
        st.tick = t;
        for(; st.cursor < a.raw.length() && a.raw[st.cursor].tick <= t; st.cursor++)
        {
            const rawevent &e = a.raw[st.cursor];
            if(e.flags&(rawevent::TEXT|rawevent::REPEAT)) continue;
            int code = inputcode(e);
            loopv(st.extras) if(st.extras[i].code == code)
            {
                if(e.kind == rawevent::KEYDOWN || e.kind == rawevent::BUTTONDOWN || e.kind == rawevent::WHEEL) st.extras[i].down = e.tick;
                if(e.kind == rawevent::KEYUP || e.kind == rawevent::BUTTONUP || e.kind == rawevent::WHEEL) st.extras[i].up = e.tick;
            }
            switch(e.kind)
            {
                case rawevent::KEYDOWN: case rawevent::BUTTONDOWN:
                {
                    if(e.kind == rawevent::KEYDOWN && opensprompt(e.bind))
                    {
                        loopv(st.extras) if(st.extras[i].code == code) st.extras[i].up = e.tick;
                        break;
                    }
                    bool have = false;
                    loopv(st.held) if(st.held[i].code == code) { st.held[i].bind = e.bind; have = true; }
                    if(!have) { heldinput &h = st.held.add(); h.code = code; h.bind = e.bind; }
                    break;
                }
                case rawevent::KEYUP: case rawevent::BUTTONUP:
                    loopvrev(st.held) if(st.held[i].code == code) st.held.remove(i);
                    break;
                case rawevent::WHEEL:
                    if(bindhas(e.bind, "jump")) st.wheeljump = e.tick;
                    break;
                case rawevent::FOCUS:
                    if(!(e.flags&rawevent::GAINED)) st.held.setsize(0);
                    break;
            }
        }
    }

    static bool actionheld(int action)
    {
        loopv(st.held) if(st.held[i].code >= 0 && bindhas(st.held[i].bind, actionbinds[action])) return true;
        return action == A_JUMP && st.tick - st.wheeljump < WHEELTICKS;
    }

    static bool buttonheld(int button)
    {
        loopv(st.held) if(st.held[i].code == -button) return true;
        return false;
    }

    // the first raw event at or after tick t
    static int firstraw(const inputattempt &a, int t)
    {
        int lo = 0, hi = a.raw.length();
        while(lo < hi)
        {
            int mid = (lo + hi) / 2;
            if(a.raw[mid].tick < t) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    // the last controls event at or before tick t, NULL if none
    static const inputevent *controlsat(const inputattempt &a, int t)
    {
        int lo = 0, hi = a.events.length();
        while(lo < hi)
        {
            int mid = (lo + hi) / 2;
            if(a.events[mid].tick <= t) lo = mid + 1; else hi = mid;
        }
        for(int i = lo - 1; i >= 0; i--) if(a.events[i].type == INPUT_CONTROLS) return &a.events[i];
        return NULL;
    }

    static float bitsfloat(uint bits) { float f; memcpy(&f, &bits, sizeof(f)); return f; }

    // how fast the view turned over the MOTIONTICKS ending at tick t, horizontal and vertical
    // together, in degrees per second
    static float turnspeed(const inputattempt &a, int t)
    {
        const inputevent *now = controlsat(a, t), *then = controlsat(a, t - MOTIONTICKS);
        if(!now || !then) return 0;
        float dyaw = bitsfloat(now->controls.yaw) - bitsfloat(then->controls.yaw), dpitch = bitsfloat(now->controls.pitch) - bitsfloat(then->controls.pitch);
        if(dyaw > 180) dyaw -= 360; else if(dyaw < -180) dyaw += 360;
        return sqrtf(dyaw*dyaw + dpitch*dpitch) * 1000.0f / (MOTIONTICKS*SIMTICK);
    }

    // the mouse movement over the MOTIONTICKS ending at tick t: raw counts, or degrees of aim
    static vec2 motionat(const inputattempt &a, int t)
    {
        vec2 m(0, 0);
        if(a.raw.length())
        {
            for(int i = firstraw(a, t - MOTIONTICKS + 1); i < a.raw.length() && a.raw[i].tick <= t; i++)
                if(a.raw[i].kind == rawevent::MOTION) { m.x += a.raw[i].dx; m.y += a.raw[i].dy; }
            return m;
        }
        const inputevent *now = controlsat(a, t), *then = controlsat(a, t - MOTIONTICKS);
        if(!now || !then) return m;
        float dyaw = bitsfloat(now->controls.yaw) - bitsfloat(then->controls.yaw), dpitch = bitsfloat(now->controls.pitch) - bitsfloat(then->controls.pitch);
        if(dyaw > 180) dyaw -= 360; else if(dyaw < -180) dyaw += 360;
        return vec2(dyaw, -dpitch);
    }

    static const int KEYBG = 0x101010, KEYBGALPHA = 150, KEYLIT = 0xF0F0F0, KEYLITALPHA = 230, KEYEDGE = 0xFFFFFF, KEYEDGEALPHA = 50;

    static void drawkey(float x, float y, float w, float h, const char *label, bool lit, float edge)
    {
        hudfillrect(x, y, x + w, y + h, lit ? KEYLIT : KEYBG, lit ? KEYLITALPHA : KEYBGALPHA);
        hudoutlinerect(x, y, x + w, y + h, edge, KEYEDGE, KEYEDGEALPHA);
        int tw = 0, th = 0;
        text_bounds(label, tw, th);
        if(!tw || !th) return;
        float scale = min(h*0.45f/th, w*0.8f/tw);
        pushhudmatrix();
        hudmatrix.translate(x + (w - tw*scale)/2, y + (h - th*scale)/2, 0);
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        int c = lit ? 0x101010 : 0xF0F0F0;
        draw_text(label, 0, 0, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255);
        pophudmatrix();
    }

    void drawreplayinputs(int w, int h)
    {
        if(!replayinputs) return;
        const inputattempt *a = NULL;
        int t = 0;
        bool live = replayviewinputs(a, t);
        if(!live && !hudediting) return;
        if(live)
        {
            advance(*a, t);
            if(!a->raw.length())
            {
                // without raw input: what the recorded controls did
                const inputevent *c = controlsat(*a, t);
                st.held.setsize(0);
                st.wheeljump = -1000;
                loopi(NUMACTIONS) copystring(st.labels[i], actiondefaults[i]);
                if(c)
                {
                    bool on[NUMACTIONS] = { c->controls.move > 0, c->controls.move < 0, c->controls.strafe > 0, c->controls.strafe < 0, c->controls.jumping };
                    loopi(NUMACTIONS) if(on[i]) { heldinput &hi = st.held.add(); hi.code = 0; hi.bind = actionbinds[i]; }
                    if(c->controls.attacking) { heldinput &hi = st.held.add(); hi.code = -1; hi.bind = NULL; }
                }
            }
        }
        else
        {
            st.attempt = NULL;
            st.held.setsize(0);
            st.wheeljump = -1000;
            loopi(NUMACTIONS) copystring(st.labels[i], actiondefaults[i]);
        }

        // layout in key widths, anchored at its centre
        float k = h*0.055f*replayinputscale, g = k*0.1f, edge = max(1.0f, k*0.03f);
        float keysw = 3*k + 2*g, mousew = 2*k + g, totalw = keysw + 2*g + mousew, layouth = 3*k + 2*g;
        // the other keys: smaller boxes as wide as their names, wrapped into rows of the layout's width
        float ek = k*0.75f;
        vector<float> extraw;
        int extrarows = 0;
        float rowx = totalw;
        loopv(st.extras)
        {
            int tw = 0, th = 0;
            text_bounds(st.extras[i].label, tw, th);
            float bw = max(ek, th ? tw*(ek*0.45f/th) + ek*0.5f : ek);
            bw = min(bw, totalw);
            if(rowx + bw > totalw) { extrarows++; rowx = 0; }
            extraw.add(bw);
            rowx += bw + g;
        }
        float totalh = layouth + (extrarows ? g + extrarows*(ek + g) - g : 0);
        float x0 = replayinputx*w - totalw/2, y0 = replayinputy*h - totalh/2;

        drawkey(x0 + k + g, y0, k, k, st.labels[A_FORWARD], live && actionheld(A_FORWARD), edge);
        drawkey(x0, y0 + k + g, k, k, st.labels[A_LEFT], live && actionheld(A_LEFT), edge);
        drawkey(x0 + k + g, y0 + k + g, k, k, st.labels[A_BACKWARD], live && actionheld(A_BACKWARD), edge);
        drawkey(x0 + 2*(k + g), y0 + k + g, k, k, st.labels[A_RIGHT], live && actionheld(A_RIGHT), edge);
        drawkey(x0, y0 + 2*(k + g), keysw, k, st.labels[A_JUMP], live && actionheld(A_JUMP), edge);

        float mx = x0 + keysw + 2*g;
        drawkey(mx, y0 + 2*(k + g), k, k, "L", live && buttonheld(1), edge);
        drawkey(mx + k + g, y0 + 2*(k + g), k, k, "R", live && buttonheld(3), edge);

        // the mouse: the dot is the movement of the last MOTIONTICKS, its trail the same a few ticks earlier
        float r = mousew/2, cx = mx + r, cy = y0 + r;
        hudfilldisc(cx, cy, r, KEYBG, KEYBGALPHA);
        hudfilldisc(cx, cy, r*0.06f, KEYEDGE, KEYEDGEALPHA*2);
        if(live) for(int j = TRAILDOTS-1; j >= 0; j--)
        {
            vec2 m = motionat(*a, t - j*TRAILSTEP);
            float len = m.magnitude();
            if(len <= 0) continue;
            float out = (r - r*0.18f)*(1 - expf(-len/st.motionscale));
            float px = cx + m.x/len*out, py = cy + m.y/len*out;
            hudfilldisc(px, py, j ? r*0.08f : r*0.14f, j ? 0xC0C0C0 : KEYLIT, j ? int(160*(1 - j/float(TRAILDOTS))) : KEYLITALPHA);
        }
        float ex = totalw, ey = y0 + layouth + g - (ek + g);
        loopv(st.extras)
        {
            if(ex + extraw[i] > totalw) { ex = 0; ey += ek + g; }
            const extrakey &x = st.extras[i];
            bool lit = live && (x.up < x.down || t - x.up < TAPTICKS);
            drawkey(x0 + ex, ey, extraw[i], ek, x.label, lit, edge);
            ex += extraw[i] + g;
        }
        defformatstring(speed, "%d deg/s", live ? int(turnspeed(*a, t) + 0.5f) : 0);
        int sw = 0, sh = 0;
        text_bounds(speed, sw, sh);
        if(sh)
        {
            float ss = min(r*0.3f/sh, r*1.3f/sw);
            pushhudmatrix();
            hudmatrix.translate(cx - sw*ss/2, cy + r*0.45f, 0);
            hudmatrix.scale(ss, ss, 1);
            flushhudmatrix();
            draw_text(speed, 0, 0, 0xF0, 0xF0, 0xF0, 200);
            pophudmatrix();
        }
        hudeditarea("replayinput", x0, y0, x0 + totalw, y0 + totalh);
    }
}
