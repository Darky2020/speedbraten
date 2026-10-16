// Speedometer: horizontal speed (units per second) of the viewed player, optionally with the
// full 3D speed next to it.

#include "game.h"

namespace game
{
    VARP(speedometer, 0, 1, 1);
    VARP(speedometer3d, 0, 1, 1);
    FVARP(speedometerscale, 0.5, 1.0, 2.0);
    FVARP(speedometerx, 0.0, 0.50, 1.0);
    FVARP(speedometery, 0.0, 0.62, 1.0);
    VARP(speedometersmooth, 0, 1, 1);
    VARP(speedometersmoothms, 10, 150, 2000);

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
        draw_text(buf, int(offset.x), int(offset.y));
        pophudmatrix();
    }

    ICOMMAND(getspeed, "i", (int *full),
    {
        fpsent *d = hudplayer();
        vec v = vec(d->vel).add(d->falling);
        intret(int((*full ? v.magnitude() : v.magnitude2()) + 0.5f));
    });
}
