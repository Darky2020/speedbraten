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

    void drawspeedometer(int w, int h)
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
        hudeditarea("speedometer", offset.x*speedometerscale, offset.y*speedometerscale, (offset.x + tw)*speedometerscale, (offset.y + th)*speedometerscale);
    }

    ICOMMAND(getspeed, "i", (int *full),
    {
        fpsent *d = hudplayer();
        vec v = vec(d->vel).add(d->falling);
        intret(int((*full ? v.magnitude() : v.magnitude2()) + 0.5f));
    });
}
