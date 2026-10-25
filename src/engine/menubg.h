// Menu background images from backgrounds/ in the home dir, drawn instead of the tiles: the chosen
// one, or a random one every menubackgroundinterval seconds, crossfading over menubackgroundfade ms.
VARP(menubackground, 0, 0, 2); // 0 tiles, 1 menubackgroundfile, 2 random
SVARP(menubackgroundfile, "");
VARP(menubackgroundinterval, 1, 30, 3600);
VARP(menubackgroundfade, 0, 1000, 10000);
VARP(menubackgroundlogo, 0, 1, 1);
VARP(menubackgrounddim, 0, 0, 90); // percent of black over the image

static vector<char *> menubgfiles;
static Texture *menubgcur = NULL, *menubgprev = NULL;
static string menubgcurfile = "";
static int menubgchanged = 0;

static bool menubgnamesort(const char *a, const char *b) { return strcmp(a, b) < 0; }

static void scanmenubackgrounds()
{
    menubgfiles.deletearrays();
    static const char * const exts[] = { "png", "jpg", "jpeg" };
    loopi(sizeof(exts)/sizeof(exts[0]))
    {
        vector<char *> names;
        listfiles("backgrounds", exts[i], names);
        loopvj(names)
        {
            defformatstring(file, "backgrounds/%s.%s", names[j], exts[i]);
            bool dup = false;
            loopvk(menubgfiles) if(!strcmp(menubgfiles[k], file)) { dup = true; break; }
            if(!dup) menubgfiles.add(newstring(file));
        }
        names.deletearrays();
    }
    menubgfiles.sort(menubgnamesort);
}
ICOMMAND(menubackgroundscan, "", (), { scanmenubackgrounds(); intret(menubgfiles.length()); });
ICOMMAND(menubackgroundfileat, "i", (int *i), result(menubgfiles.inrange(*i) ? menubgfiles[*i] : ""));

static void switchmenubackground(const char *file)
{
    Texture *t = file[0] ? textureload(file, 3, true, false) : NULL;
    if(t == notexture) t = NULL;
    copystring(menubgcurfile, file);
    if(t == menubgcur) return;
    menubgprev = menubgcur;
    menubgcur = t;
    menubgchanged = totalmillis;
}

// picks the image to show now; false to draw the tiles
static bool updatemenubackground()
{
    if(menubackground == 1)
    {
        if(strcmp(menubgcurfile, menubackgroundfile)) switchmenubackground(menubackgroundfile);
    }
    else if(menubackground == 2)
    {
        if(!menubgcur || totalmillis - menubgchanged >= menubackgroundinterval*1000)
        {
            if(menubgfiles.empty()) scanmenubackgrounds();
            vector<const char *> choices;
            loopv(menubgfiles) if(strcmp(menubgfiles[i], menubgcurfile)) choices.add(menubgfiles[i]);
            if(choices.length()) switchmenubackground(choices[crnd(choices.length())]);
            else if(!menubgcur && menubgfiles.length()) switchmenubackground(menubgfiles[0]);
            else menubgchanged = totalmillis;
        }
    }
    return menubackground && menubgcur;
}

static float menubackgroundalpha()
{
    int age = totalmillis - menubgchanged;
    return menubackgroundfade <= 0 || age >= menubackgroundfade || age < 0 ? 1 : age / float(menubackgroundfade);
}

// a texture scaled to cover w x h, centred, keeping its aspect
static void covermenubackground(Texture *t, int w, int h, float alpha)
{
    float scale = max(w / float(max(t->xs, 1)), h / float(max(t->ys, 1))), tw = t->xs*scale, th = t->ys*scale;
    gle::colorf(1, 1, 1, alpha);
    glBindTexture(GL_TEXTURE_2D, t->id);
    bgquad(0.5f*(w - tw), 0.5f*(h - th), tw, th);
    gle::colorf(1, 1, 1);
}
