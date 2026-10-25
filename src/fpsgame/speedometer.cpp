// Speedometer: horizontal speed (units per second) of the viewed player, optionally with the
// full 3D speed next to it.

#include "game.h"
#include "sbcompat.h"

namespace game
{
    MOD(VARP, speedometer, 0, 1, 1);
    MOD(VARP, speedometer3d, 0, 1, 1);
    MOD(FVARP, speedometerscale, 0.5, 1.0, 2.0);
    MOD(FVARP, speedometerx, 0.0, 0.50, 1.0);
    MOD(FVARP, speedometery, 0.0, 0.62, 1.0);
    MOD(VARP, speedometersmooth, 0, 1, 1);
    MOD(VARP, speedometersmoothms, 10, 150, 2000);

    // Colour bands by horizontal speed, saved as "speed RRGGBB" pairs: a band's colour is used from
    // its speed up to the next band's.
    struct speedband { int speed, colour; };
    static vector<speedband> bands;
    static bool bandsparsed = false;
    static void parsebands();
    MOD(SVARFP, speedometerbands, "0 FFFFFF", parsebands());

    static bool slowerband(const speedband &a, const speedband &b) { return a.speed < b.speed; }

    static void parsebands()
    {
        bandsparsed = true;
        bands.setsize(0);
        vector<char *> w;
        explodelist(speedometerbands, w);
        for(int i = 0; i + 1 < w.length(); i += 2)
        {
            speedband &b = bands.add();
            b.speed = max(parseint(w[i]), 0);
            b.colour = int(strtoul(w[i+1], NULL, 16)) & 0xFFFFFF;
        }
        w.deletearrays();
        bands.sort(slowerband);
    }

    // the setting's change hook doesn't run for its default value
    static vector<speedband> &getbands()
    {
        if(!bandsparsed) parsebands();
        return bands;
    }

    static void savebands()
    {
        bands.sort(slowerband);
        vector<char> buf;
        loopv(bands)
        {
            defformatstring(pair, "%s%d %06X", i ? " " : "", bands[i].speed, bands[i].colour);
            buf.put(pair, strlen(pair));
        }
        buf.add('\0');
        setsvar("speedometerbands", buf.getbuf());
    }

    static int bandcolour(float speed)
    {
        int colour = 0xFFFFFF;
        getbands();
        loopv(bands) if(speed + 0.5f >= bands[i].speed) colour = bands[i].colour;
        return colour;
    }

    ICOMMAND(speedometerbandcount, "", (), intret(getbands().length()));
    // speedometerband i speed|colour
    ICOMMAND(speedometerband, "is", (int *i, char *field),
    {
        if(!getbands().inrange(*i)) { result(""); return; }
        if(!strcmp(field, "speed")) intret(bands[*i].speed);
        else { defformatstring(hex, "%06X", bands[*i].colour); result(hex); }
    });
    // speedometerbandset i speed RRGGBB
    ICOMMAND(speedometerbandset, "iis", (int *i, int *speed, char *colour),
    {
        if(!getbands().inrange(*i)) return;
        bands[*i].speed = max(*speed, 0);
        bands[*i].colour = int(strtoul(colour, NULL, 16)) & 0xFFFFFF;
        savebands();
    });
    ICOMMAND(speedometerbandadd, "", (),
    {
        speedband &b = getbands().add();
        b.speed = bands.length() > 1 ? bands[bands.length()-2].speed + 100 : 0;
        b.colour = 0xFFFFFF;
        savebands();
    });
    ICOMMAND(speedometerbandremove, "i", (int *i), { if(getbands().inrange(*i)) { bands.remove(*i); savebands(); } });
    ICOMMAND(speedometerbandcolour, "f", (float *speed), { defformatstring(hex, "%06X", bandcolour(*speed)); result(hex); });

    // Speed changes a little on almost every tick while strafing; optionally show an
    // exponential average over speedometersmoothms of real time instead.
    static float smoothspeed2 = 0, smoothspeed3 = 0;
    static int lastsmoothmillis = 0;
    static fpsent *lastsmoothplayer = NULL;

    static void drawspeed(int w, int h)
    {
        fpsent *d = hudplayer();
        if(!speedometer || d->state == CS_EDITING || d->state == CS_SPECTATOR) return;
        vec v = vec(d->vel).add(d->falling);
        float speed2 = v.magnitude2(), speed3 = v.magnitude();
        if(speedometersmooth)
        {
            int dt = totalmillis - lastsmoothmillis;
            if(d != lastsmoothplayer || dt < 0 || dt > 1000) { smoothspeed2 = speed2; smoothspeed3 = speed3; }
            else
            {
                float k = 1 - expf(-dt / float(speedometersmoothms));
                smoothspeed2 += (speed2 - smoothspeed2) * k;
                smoothspeed3 += (speed3 - smoothspeed3) * k;
            }
            lastsmoothmillis = totalmillis;
            lastsmoothplayer = d;
            speed2 = smoothspeed2;
            speed3 = smoothspeed3;
        }
        string buf;
        if(speedometer3d) formatstring(buf, "%d \f4%d", int(speed2 + 0.5f), int(speed3 + 0.5f));
        else formatstring(buf, "%d", int(speed2 + 0.5f));
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);
        vec2 offset = vec2(speedometerx, speedometery).mul(vec2(w, h).div(speedometerscale));
        offset.x -= tw/2.0f;
        offset.y -= th/2.0f;

        pushhudmatrix();
        hudmatrix.scale(speedometerscale, speedometerscale, 1);
        flushhudmatrix();
        int colour = bandcolour(speed2);
        draw_text(buf, int(offset.x), int(offset.y), (colour>>16)&0xFF, (colour>>8)&0xFF, colour&0xFF, 255);
        pophudmatrix();
    }

    // The info panel: lines the player picks (a campaign run's deaths, position, view angles,
    // speed), right-aligned, on top of the engine's fps and clock in the bottom right corner or
    // at infopanelx/y.
    MOD(VARP, infopanel, 0, 1, 1);
    MOD(VARP, infopanelstack, 0, 1, 1);
    MOD(FVARP, infopanelscale, 0.5, 1.0, 2.0);
    MOD(FVARP, infopanelx, 0.0, 0.98, 1.0);
    MOD(FVARP, infopanely, 0.0, 0.80, 1.0);
    MOD(HVARP, infopanelcolour, 0, 0xB0B0B0, 0xFFFFFF);
    MOD(VARP, infopaneldeaths, 0, 1, 1);
    MOD(VARP, infopanelposition, 0, 0, 1);
    MOD(VARP, infopanelangles, 0, 0, 1);
    MOD(VARP, infopanelspeed, 0, 0, 1);

    static int infopanellines(string *lines)
    {
        int n = 0;
        if(!infopanel) return 0;
        fpsent *d = hudplayer();
        int deaths = runtimerdeaths();
        if(infopaneldeaths && deaths > 0) formatstring(lines[n++], "%d death%s", deaths, deaths == 1 ? "" : "s");
        if(infopanelposition) formatstring(lines[n++], "(%d, %d, %d)", int(floorf(d->o.x)), int(floorf(d->o.y)), int(floorf(d->o.z)));
        if(infopanelangles) formatstring(lines[n++], "(%.1f, %.1f)", d->yaw, d->pitch);
        if(infopanelspeed && d->state != CS_EDITING && d->state != CS_SPECTATOR)
        {
            vec v = vec(d->vel).add(d->falling);
            formatstring(lines[n++], "speed %d", int(v.magnitude2() + 0.5f));
        }
        return n;
    }

    static void drawinfolines(string *lines, int n, int right, int bottom)
    {
        int y = bottom;
        for(int i = n-1; i >= 0; i--)
        {
            int tw = 0, th = 0;
            text_bounds(lines[i], tw, th);
            y -= th;
            draw_text(lines[i], right - tw, y, (infopanelcolour>>16)&0xFF, (infopanelcolour>>8)&0xFF, infopanelcolour&0xFF, 255);
        }
    }

    // on top of the engine's bottom right stack
    void hudstack(int right, int top)
    {
        if(!infopanelstack) return;
        string lines[4];
        int n = infopanellines(lines);
        if(!n) return;
        int tw = 0, th = 0;
        text_bounds("0", tw, th);
        drawinfolines(lines, n, right, top + th);
    }

    void drawspeedometer(int w, int h)
    {
        drawspeed(w, h);
        if(infopanelstack) return;
        string lines[4];
        int n = infopanellines(lines);
        if(!n) return;
        const float scale = 0.4f*infopanelscale;
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        drawinfolines(lines, n, int(infopanelx*w/scale), int(infopanely*h/scale));
        pophudmatrix();
    }

    ICOMMAND(getspeed, "i", (int *full),
    {
        fpsent *d = hudplayer();
        vec v = vec(d->vel).add(d->falling);
        intret(int((*full ? v.magnitude() : v.magnitude2()) + 0.5f));
    });
}
