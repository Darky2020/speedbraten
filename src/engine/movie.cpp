// Feedback on playing videos:
//   quicktime - ok
//   vlc - ok
//   xine - ok
//   mplayer - ok
//   totem - ok
//   avidemux - ok - 3Apr09-RockKeyman:had to swap UV channels as it showed up blue
//   kino - ok

#include "engine.h"
#ifdef __APPLE__
  #include <SDL2_mixer/SDL_mixer.h>
#else
#include "SDL_mixer.h"
#endif

#include <sys/stat.h>
#ifndef GL_MAX_RENDERBUFFER_SIZE
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#endif
#ifndef WIN32
#include <signal.h>
#endif

VAR(dbgmovie, 0, 0, 1);

// with ffmpeg found, frames are piped to it and encoded as H.264; without it (or for a .avi name) the raw I420 AVI is written
VARP(movieffmpeg, 0, 1, 1);
SVARP(movieffmpegpath, "");
VARP(moviequality, 1, 50, 100);

#ifdef WIN32
#define moviepopen _popen
#define moviepclose _pclose
#else
#define moviepopen popen
#define moviepclose pclose
#endif

typedef char cmdline[4096];

static void quotearg(cmdline &dst, const char *arg)
{
#ifdef WIN32
    formatstring(dst, "\"%s\"", arg);
#else
    char *d = dst, *end = &dst[sizeof(cmdline)-5];
    *d++ = '\'';
    for(const char *c = arg; *c && d < end; c++)
    {
        if(*c == '\'') { if(d + 4 >= end) break; memcpy(d, "'\\''", 4); d += 4; }
        else *d++ = *c;
    }
    *d++ = '\'';
    *d = '\0';
#endif
}

static int runffmpeg(const char *args, const char *mode, FILE **pipe = NULL, char *out = NULL, int outlen = 0);

static string ffmpegbin = "";
static const char *ffmpegencoder = NULL;
// the last movie rendered through ffmpeg, which a sound pass is added to
static string lastrender = "";
// the last rendered movie, also after its sound pass: the full path and the name it was asked for
static string lastmovie = "", lastmoviename = "";
static FILE *soundpassfile = NULL;

// finds an ffmpeg with an H.264 encoder, preferring the hardware one on macOS
static bool findffmpeg()
{
    const char *paths[] = { movieffmpegpath, "ffmpeg",
#ifdef __APPLE__
        "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg",
#endif
    };
    loopi(sizeof(paths)/sizeof(paths[0]))
    {
        if(!paths[i][0]) continue;
        copystring(ffmpegbin, paths[i]);
        char encoders[1<<16];
        if(runffmpeg("-hide_banner -encoders", "r", NULL, encoders, sizeof(encoders)) != 0) continue;
#ifdef __APPLE__
        if(strstr(encoders, " h264_videotoolbox ")) { ffmpegencoder = "h264_videotoolbox"; return true; }
#endif
        if(strstr(encoders, " libx264 ")) { ffmpegencoder = "libx264"; return true; }
    }
    ffmpegbin[0] = '\0';
    ffmpegencoder = NULL;
    return false;
}

// runs ffmpegbin with args: with pipe, leaves it open for writing; with out, reads its output and returns its exit status
static int runffmpeg(const char *args, const char *mode, FILE **pipe, char *out, int outlen)
{
    cmdline bin, cmd, wcmd;
    quotearg(bin, ffmpegbin);
    formatstring(cmd, "%s %s", bin, args);
    // cmd.exe drops the outer quotes of a line that starts with one
#ifdef WIN32
    formatstring(wcmd, pipe || dbgmovie ? "\"%s\"" : "\"%s 2>nul\"", cmd);
#else
    formatstring(wcmd, pipe || dbgmovie ? "%s" : "%s 2>/dev/null", cmd);
#endif
    if(dbgmovie) conoutf(CON_DEBUG, "ffmpeg: %s", wcmd);
#ifdef WIN32
    FILE *p = moviepopen(wcmd, mode[0] == 'w' ? "wb" : "rb");
#else
    FILE *p = moviepopen(wcmd, mode);
#endif
    if(!p) return -1;
    if(pipe) { *pipe = p; return 0; }
    int len = 0;
    if(out && outlen > 0)
    {
        for(size_t n; len < outlen-1 && (n = fread(&out[len], 1, outlen-1-len, p)) > 0;) len += n;
        out[len] = '\0';
    }
    char discard[4096];
    while(fread(discard, 1, sizeof(discard), p) > 0);
    int status = moviepclose(p);
    if(dbgmovie) conoutf(CON_DEBUG, "ffmpeg: exit %d", status);
    return status;
}

struct aviindexentry
{
    int frame, type, size;
    uint offset;

    aviindexentry() {}
    aviindexentry(int frame, int type, int size, uint offset) : frame(frame), type(type), size(size), offset(offset) {}
};

struct avisegmentinfo
{
    stream::offset offset, videoindexoffset, soundindexoffset;
    int firstindex;
    uint videoindexsize, soundindexsize, indexframes, videoframes, soundframes;
    
    avisegmentinfo() {}
    avisegmentinfo(stream::offset offset, int firstindex) : offset(offset), videoindexoffset(0), soundindexoffset(0), firstindex(firstindex), videoindexsize(0), soundindexsize(0), indexframes(0), videoframes(0), soundframes(0) {}
};

struct aviwriter
{
    stream *f;
    FILE *pipe, *soundf;
    string videoname, soundname;
    uchar *yuv;
    uint videoframes;
    stream::offset totalsize;
    const uint videow, videoh, videofps;
    string filename;
 
    int soundfrequency, soundchannels;
    Uint16 soundformat;
    
    vector<aviindexentry> index;
    vector<avisegmentinfo> segments;
    
    stream::offset fileframesoffset, fileextframesoffset, filevideooffset, filesoundoffset, superindexvideooffset, superindexsoundoffset;
    
    enum { MAX_CHUNK_DEPTH = 16, MAX_SUPER_INDEX = 1024 };
    stream::offset chunkoffsets[MAX_CHUNK_DEPTH];
    int chunkdepth;
    
    aviindexentry &addindex(int frame, int type, int size)
    {
        avisegmentinfo &seg = segments.last();
        int i = index.length();
        while(--i >= seg.firstindex)
        {
            aviindexentry &e = index[i];
            if(frame > e.frame || (frame == e.frame && type <= e.type)) break;
        }
        return index.insert(i + 1, aviindexentry(frame, type, size, uint(totalsize - chunkoffsets[chunkdepth])));
    }
    
    double filespaceguess() 
    {
        if(pipe)
        {
            struct stat st;
            return stat(videoname, &st) == 0 ? double(st.st_size) + (soundf ? double(ftell(soundf)) : 0) : 0;
        }
        return double(totalsize);
    }
       
    void startchunk(const char *fcc, uint size = 0)
    {
        f->write(fcc, 4);
        f->putlil<uint>(size);
        totalsize += 4 + 4;
        chunkoffsets[++chunkdepth] = totalsize;
        totalsize += size;
    }
    
    void listchunk(const char *fcc, const char *lfcc)
    {
        startchunk(fcc);
        f->write(lfcc, 4);
        totalsize += 4;
    }
    
    void endchunk()
    {
        ASSERT(chunkdepth >= 0);
        --chunkdepth;
    }

    void endlistchunk()
    {
        ASSERT(chunkdepth >= 0);
        int size = int(totalsize - chunkoffsets[chunkdepth]);
        f->seek(-4 - size, SEEK_CUR);
        f->putlil(size);
        f->seek(0, SEEK_END);
        if(size & 1) { f->putchar(0x00); totalsize++; }
        endchunk();
    }
        
    void writechunk(const char *fcc, const void *data, uint len) // simplify startchunk()/endchunk() to avoid f->seek()
    {
        f->write(fcc, 4);
        f->putlil(len);
        f->write(data, len);
        totalsize += 4 + 4 + len;
        if(len & 1) { f->putchar(0x00); totalsize++; }
    }
    
    const char *rawsoundformat()
    {
        switch(soundformat)
        {
            case AUDIO_U8: case AUDIO_S8: return "s8";
            case AUDIO_U16LSB: case AUDIO_U16MSB: case AUDIO_S16LSB: case AUDIO_S16MSB: return "s16le";
            case AUDIO_F32LSB: return "f32le";
            default: return NULL;
        }
    }

    void closepipe()
    {
        moviepclose(pipe);
        pipe = NULL;
        bool hassound = soundf && ftell(soundf) > 0;
        if(soundf) { fclose(soundf); soundf = NULL; }
        if(hassound)
        {
            cmdline video, sound, out, args;
            quotearg(video, videoname);
            quotearg(sound, soundname);
            quotearg(out, findfile(filename, "wb"));
            formatstring(args, "-y -loglevel error -i %s -f %s -ar %d -ac %d -i %s -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -shortest -movflags +faststart %s",
                video, rawsoundformat(), soundfrequency, soundchannels, sound, out);
            if(runffmpeg(args, "r") == 0) remove(videoname);
            else conoutf(CON_ERROR, "could not add the sound to %s; video only in %s", filename, videoname);
        }
        else
        {
            const char *out = findfile(filename, "wb");
            remove(out);
            rename(videoname, out);
        }
        remove(soundname);
    }

    void close()
    {
        if(pipe) { closepipe(); return; }
        if(!f) return;
        flushsegment();

        uint soundindexes = 0, videoindexes = 0, soundframes = 0, videoframes = 0, indexframes = 0;
        loopv(segments)
        {
            avisegmentinfo &seg = segments[i];
            if(seg.soundindexsize) soundindexes++;
            videoindexes++;
            soundframes += seg.soundframes;
            videoframes += seg.videoframes;
            indexframes += seg.indexframes;
        }
        if(dbgmovie) conoutf(CON_DEBUG, "fileframes: sound=%d, video=%d+%d(dups)\n", soundframes, videoframes, indexframes-videoframes);
        f->seek(fileframesoffset, SEEK_SET);
        f->putlil<uint>(segments[0].indexframes);
        f->seek(filevideooffset, SEEK_SET);
        f->putlil<uint>(segments[0].videoframes);
        if(segments[0].soundframes > 0)
        {
            f->seek(filesoundoffset, SEEK_SET);
            f->putlil<uint>(segments[0].soundframes);
        }
        f->seek(fileextframesoffset, SEEK_SET);
        f->putlil<uint>(indexframes); // total video frames

        f->seek(superindexvideooffset + 2 + 2, SEEK_SET);
        f->putlil<uint>(videoindexes);
        f->seek(superindexvideooffset + 2 + 2 + 4 + 4 + 4 + 4 + 4, SEEK_SET);
        loopv(segments)
        {
            avisegmentinfo &seg = segments[i];
            f->putlil<uint>(seg.videoindexoffset&stream::offset(0xFFFFFFFFU));
            f->putlil<uint>(seg.videoindexoffset>>32);
            f->putlil<uint>(seg.videoindexsize);
            f->putlil<uint>(seg.indexframes);
        }

        if(soundindexes > 0)
        {
            f->seek(superindexsoundoffset + 2 + 2, SEEK_SET);
            f->putlil<uint>(soundindexes);
            f->seek(superindexsoundoffset + 2 + 2 + 4 + 4 + 4 + 4 + 4, SEEK_SET);
            loopv(segments)
            {
                avisegmentinfo &seg = segments[i];
                if(!seg.soundindexsize) continue;
                f->putlil<uint>(seg.soundindexoffset&stream::offset(0xFFFFFFFFU));
                f->putlil<uint>(seg.soundindexoffset>>32);
                f->putlil<uint>(seg.soundindexsize);
                f->putlil<uint>(seg.soundframes);
            }
        }

        f->seek(0, SEEK_END);
        
        DELETEP(f);
    }
    
    aviwriter(const char *name, uint w, uint h, uint fps, bool sound) : f(NULL), pipe(NULL), soundf(NULL), yuv(NULL), videoframes(0), totalsize(0), videow(w&~1), videoh(h&~1), videofps(fps), soundfrequency(0),soundchannels(0),soundformat(0)
    {
        copystring(filename, name);
        path(filename);
        const char *ext = strrchr(filename, '.');
        if(!movieffmpeg || (ext && !strcasecmp(ext, ".avi")) || !findffmpeg()) ffmpegencoder = NULL;
        if(!ext) concatstring(filename, ffmpegencoder ? ".mp4" : ".avi");
        
        extern bool nosound; // sound.cpp
        if(sound && !nosound) 
        {
            Mix_QuerySpec(&soundfrequency, &soundformat, &soundchannels);
            const char *desc;
            switch(soundformat)
            {
                case AUDIO_U8:     desc = "u8"; break;
                case AUDIO_S8:     desc = "s8"; break;
                case AUDIO_U16LSB: desc = "u16l"; break;
                case AUDIO_U16MSB: desc = "u16b"; break;
                case AUDIO_S16LSB: desc = "s16l"; break;
                case AUDIO_S16MSB: desc = "s16b"; break;
                default:           desc = "unkn";
            }
            if(dbgmovie) conoutf(CON_DEBUG, "soundspec: %dhz %s x %d", soundfrequency, desc, soundchannels);
        }
    }
    
    ~aviwriter()
    {
        close();
        if(yuv) delete [] yuv;
    }
    
    bool openpipe()
    {
        const char *out = findfile(filename, "wb");
        formatstring(videoname, "%s.video%s", out, strrchr(filename, '.'));
        formatstring(soundname, "%s.sound", out);
        if(soundfrequency > 0 && !rawsoundformat()) soundfrequency = 0;
        if(soundfrequency > 0)
        {
            soundf = fopen(soundname, "wb");
            if(!soundf) return false;
        }
        cmdline video, args;
        quotearg(video, videoname);
        string quality;
        if(!strcmp(ffmpegencoder, "libx264")) formatstring(quality, "-preset veryfast -crf %d", clamp(45 - moviequality/2, 0, 51));
        else formatstring(quality, "-q:v %d", moviequality);
        formatstring(args, "-y -loglevel error -nostats -f rawvideo -pix_fmt yuv420p -s %dx%d -r %d -i - -c:v %s %s -pix_fmt yuv420p -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv -movflags +faststart %s",
            videow, videoh, videofps, ffmpegencoder, quality, video);
#ifndef WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        if(runffmpeg(args, "w", &pipe) != 0 || !pipe)
        {
            pipe = NULL;
            if(soundf) { fclose(soundf); soundf = NULL; remove(soundname); }
            return false;
        }
        return true;
    }

    bool open()
    {
        if(ffmpegencoder) return openpipe();
        f = openfile(filename, "wb");
        if(!f) return false;
        
        chunkdepth = -1;
        
        listchunk("RIFF", "AVI ");
        
        listchunk("LIST", "hdrl");
        
        startchunk("avih", 56);
        f->putlil<uint>(1000000 / videofps); // microsecsperframe
        f->putlil<uint>(0); // maxbytespersec
        f->putlil<uint>(0); // reserved
        f->putlil<uint>(0x10 | 0x20); // flags - hasindex|mustuseindex
        fileframesoffset = f->tell();
        f->putlil<uint>(0); // totalvideoframes
        f->putlil<uint>(0); // initialframes
        f->putlil<uint>(soundfrequency > 0 ? 2 : 1); // streams
        f->putlil<uint>(0); // buffersize
        f->putlil<uint>(videow); // video width
        f->putlil<uint>(videoh); // video height
        loopi(4) f->putlil<uint>(0); // reserved
        endchunk(); // avih
        
        listchunk("LIST", "strl");
        
        startchunk("strh", 56);
        f->write("vids", 4); // fcctype
        f->write("I420", 4); // fcchandler
        f->putlil<uint>(0); // flags
        f->putlil<uint>(0); // priority
        f->putlil<uint>(0); // initialframes
        f->putlil<uint>(1); // scale
        f->putlil<uint>(videofps); // rate
        f->putlil<uint>(0); // start
        filevideooffset = f->tell();
        f->putlil<uint>(0); // length
        f->putlil<uint>(videow*videoh*3/2); // suggested buffersize
        f->putlil<uint>(0); // quality
        f->putlil<uint>(0); // samplesize
        f->putlil<ushort>(0); // frame left
        f->putlil<ushort>(0); // frame top
        f->putlil<ushort>(videow); // frame right
        f->putlil<ushort>(videoh); // frame bottom
        endchunk(); // strh
        
        startchunk("strf", 40);
        f->putlil<uint>(40); //headersize
        f->putlil<uint>(videow); // width
        f->putlil<uint>(videoh); // height
        f->putlil<ushort>(3); // planes
        f->putlil<ushort>(12); // bitcount
        f->write("I420", 4); // compression
        f->putlil<uint>(videow*videoh*3/2); // imagesize
        f->putlil<uint>(0); // xres
        f->putlil<uint>(0); // yres;
        f->putlil<uint>(0); // colorsused
        f->putlil<uint>(0); // colorsrequired
        endchunk(); // strf
       
        startchunk("indx", 24 + 16*MAX_SUPER_INDEX);
        superindexvideooffset = f->tell();
        f->putlil<ushort>(4); // longs per entry
        f->putlil<ushort>(0); // index of indexes
        f->putlil<uint>(0); // entries in use
        f->write("00dc", 4); // chunk id
        f->putlil<uint>(0); // reserved 1
        f->putlil<uint>(0); // reserved 2
        f->putlil<uint>(0); // reserved 3
        loopi(MAX_SUPER_INDEX)
        {
            f->putlil<uint>(0); // offset low
            f->putlil<uint>(0); // offset high
            f->putlil<uint>(0); // size
            f->putlil<uint>(0); // duration
        }
        endchunk(); // indx

        startchunk("vprp", 68);
        f->putlil<uint>(0); // video format token
        f->putlil<uint>(0); // video standard
        f->putlil<uint>(videofps); // vertical refresh rate
        f->putlil<uint>(videow); // horizontal total
        f->putlil<uint>(videoh); // vertical total
        int gcd = screenw, rem = screenh;
        while(rem > 0) { gcd %= rem; swap(gcd, rem); }
        f->putlil<ushort>(screenh/gcd); // aspect denominator
        f->putlil<ushort>(screenw/gcd); // aspect numerator
        f->putlil<uint>(videow); // frame width
        f->putlil<uint>(videoh); // frame height
        f->putlil<uint>(1); // fields per frame
        f->putlil<uint>(videoh); // compressed bitmap height
        f->putlil<uint>(videow); // compressed bitmap width
        f->putlil<uint>(videoh); // valid bitmap height
        f->putlil<uint>(videow); // valid bitmap width
        f->putlil<uint>(0); // valid bitmap x offset
        f->putlil<uint>(0); // valid bitmap y offset
        f->putlil<uint>(0); // video x offset
        f->putlil<uint>(0); // video y start
        endchunk(); // vprp

        endlistchunk(); // LIST strl
                
        if(soundfrequency > 0)
        {
            const int bps = (soundformat==AUDIO_U8 || soundformat == AUDIO_S8) ? 1 : 2;
            
            listchunk("LIST", "strl");
            
            startchunk("strh", 56);
            f->write("auds", 4); // fcctype
            f->putlil<uint>(1); // fcchandler - normally 4cc, but audio is a special case
            f->putlil<uint>(0); // flags
            f->putlil<uint>(0); // priority
            f->putlil<uint>(0); // initialframes
            f->putlil<uint>(1); // scale
            f->putlil<uint>(soundfrequency); // rate
            f->putlil<uint>(0); // start
            filesoundoffset = f->tell();
            f->putlil<uint>(0); // length
            f->putlil<uint>(soundfrequency*bps*soundchannels/2); // suggested buffer size (this is a half second)
            f->putlil<uint>(0); // quality
            f->putlil<uint>(bps*soundchannels); // samplesize
            f->putlil<ushort>(0); // frame left
            f->putlil<ushort>(0); // frame top
            f->putlil<ushort>(0); // frame right
            f->putlil<ushort>(0); // frame bottom
            endchunk(); // strh
            
            startchunk("strf", 18);
            f->putlil<ushort>(1); // format (uncompressed PCM)
            f->putlil<ushort>(soundchannels); // channels
            f->putlil<uint>(soundfrequency); // sampleframes per second
            f->putlil<uint>(soundfrequency*bps*soundchannels); // average bytes per second
            f->putlil<ushort>(bps*soundchannels); // block align <-- guess
            f->putlil<ushort>(bps*8); // bits per sample
            f->putlil<ushort>(0); // size
            endchunk(); //strf

            startchunk("indx", 24 + 16*MAX_SUPER_INDEX);
            superindexsoundoffset = f->tell();
            f->putlil<ushort>(4); // longs per entry
            f->putlil<ushort>(0); // index of indexes
            f->putlil<uint>(0); // entries in use
            f->write("01wb", 4); // chunk id
            f->putlil<uint>(0); // reserved 1
            f->putlil<uint>(0); // reserved 2
            f->putlil<uint>(0); // reserved 3
            loopi(MAX_SUPER_INDEX)
            {
                f->putlil<uint>(0); // offset low
                f->putlil<uint>(0); // offset high
                f->putlil<uint>(0); // size
                f->putlil<uint>(0); // duration
            }
            endchunk(); // indx

            endlistchunk(); // LIST strl
        }
       
        listchunk("LIST", "odml");
        startchunk("dmlh", 4);
        fileextframesoffset = f->tell();
        f->putlil<uint>(0);
        endchunk(); // dmlh
        endlistchunk(); // LIST odml

        listchunk("LIST", "INFO");
        const char *software = "Cube 2: Sauerbraten";
        writechunk("ISFT", software, strlen(software)+1);
        endlistchunk(); // LIST INFO
        
        endlistchunk(); // LIST hdrl
        
        nextsegment();
 
        return true;
    }
  
    static inline void boxsample(const uchar *src, const uint stride, 
                                 const uint area, const uint w, uint h, 
                                 const uint xlow, const uint xhigh, const uint ylow, const uint yhigh,
                                 uint &bdst, uint &gdst, uint &rdst)
    {
        const uchar *end = &src[w<<2];
        uint bt = 0, gt = 0, rt = 0;
        for(const uchar *cur = &src[4]; cur < end; cur += 4)
        {
            bt += cur[0];
            gt += cur[1];
            rt += cur[2];
        }
        bt = ylow*(bt + ((src[0]*xlow + end[0]*xhigh)>>12));
        gt = ylow*(gt + ((src[1]*xlow + end[1]*xhigh)>>12));
        rt = ylow*(rt + ((src[2]*xlow + end[2]*xhigh)>>12));
        if(h) 
        {
            for(src += stride, end += stride; --h; src += stride, end += stride)  
            {
                uint b = 0, g = 0, r = 0;
                for(const uchar *cur = &src[4]; cur < end; cur += 4)
                {
                    b += cur[0];
                    g += cur[1];
                    r += cur[2];
                }
                bt += (b<<12) + src[0]*xlow + end[0]*xhigh;
                gt += (g<<12) + src[1]*xlow + end[1]*xhigh;
                rt += (r<<12) + src[2]*xlow + end[2]*xhigh;
            }
            uint b = 0, g = 0, r = 0;
            for(const uchar *cur = &src[4]; cur < end; cur += 4)
            {
                b += cur[0];
                g += cur[1];
                r += cur[2];
            }
            bt += yhigh*(b + ((src[0]*xlow + end[0]*xhigh)>>12));
            gt += yhigh*(g + ((src[1]*xlow + end[1]*xhigh)>>12));
            rt += yhigh*(r + ((src[2]*xlow + end[2]*xhigh)>>12));
        }
        bdst = (bt*area)>>24;
        gdst = (gt*area)>>24;
        rdst = (rt*area)>>24;
    }
 
    void scaleyuv(const uchar *pixels, uint srcw, uint srch)
    {
        const int flip = -1;
        const uint planesize = videow * videoh;
        if(!yuv) yuv = new uchar[(planesize*3)/2];
        uchar *yplane = yuv, *uplane = yuv + planesize, *vplane = yuv + planesize + planesize/4;
        const int ystride = flip*int(videow), uvstride = flip*int(videow)/2;
        if(flip < 0) { yplane -= int(videoh-1)*ystride; uplane -= int(videoh/2-1)*uvstride; vplane -= int(videoh/2-1)*uvstride; }

        const uint stride = srcw<<2;
        srcw &= ~1;
        srch &= ~1;
        const uint wfrac = (srcw<<12)/videow, hfrac = (srch<<12)/videoh, 
                   area = ((ullong)planesize<<12)/(srcw*srch + 1),
                   dw = videow*wfrac, dh = videoh*hfrac;
  
        for(uint y = 0; y < dh;)
        {
            uint yn = y + hfrac - 1, yi = y>>12, h = (yn>>12) - yi, ylow = ((yn|(-int(h)>>24))&0xFFFU) + 1 - (y&0xFFFU), yhigh = (yn&0xFFFU) + 1;
            y += hfrac;
            uint y2n = y + hfrac - 1, y2i = y>>12, h2 = (y2n>>12) - y2i, y2low = ((y2n|(-int(h2)>>24))&0xFFFU) + 1 - (y&0xFFFU), y2high = (y2n&0xFFFU) + 1;
            y += hfrac;

            const uchar *src = &pixels[yi*stride], *src2 = &pixels[y2i*stride];
            uchar *ydst = yplane, *ydst2 = yplane + ystride, *udst = uplane, *vdst = vplane;
            for(uint x = 0; x < dw;)
            {
                uint xn = x + wfrac - 1, xi = x>>12, w = (xn>>12) - xi, xlow = ((w+0xFFFU)&0x1000U) - (x&0xFFFU), xhigh = (xn&0xFFFU) + 1;
                x += wfrac;
                uint x2n = x + wfrac - 1, x2i = x>>12, w2 = (x2n>>12) - x2i, x2low = ((w2+0xFFFU)&0x1000U) - (x&0xFFFU), x2high = (x2n&0xFFFU) + 1;
                x += wfrac;

                uint b1, g1, r1, b2, g2, r2, b3, g3, r3, b4, g4, r4;
                boxsample(&src[xi<<2], stride, area, w, h, xlow, xhigh, ylow, yhigh, b1, g1, r1);
                boxsample(&src[x2i<<2], stride, area, w2, h, x2low, x2high, ylow, yhigh, b2, g2, r2);
                boxsample(&src2[xi<<2], stride, area, w, h2, xlow, xhigh, y2low, y2high, b3, g3, r3);
                boxsample(&src2[x2i<<2], stride, area, w2, h2, x2low, x2high, y2low, y2high, b4, g4, r4);


                // Y  = 16 + 65.481*R + 128.553*G + 24.966*B
                // Cb = 128 - 37.797*R - 74.203*G + 112.0*B
                // Cr = 128 + 112.0*R - 93.786*G - 18.214*B
                *ydst++ = ((16<<12) + 1052*r1 + 2065*g1 + 401*b1)>>12;
                *ydst++ = ((16<<12) + 1052*r2 + 2065*g2 + 401*b2)>>12;
                *ydst2++ = ((16<<12) + 1052*r3 + 2065*g3 + 401*b3)>>12;;
                *ydst2++ = ((16<<12) + 1052*r4 + 2065*g4 + 401*b4)>>12;;

                const uint b = b1 + b2 + b3 + b4,
                           g = g1 + g2 + g3 + g4,
                           r = r1 + r2 + r3 + r4;
                // note: weights here are scaled by 1<<10, as opposed to 1<<12, since r/g/b are already *4
                *udst++ = ((128<<12) - 152*r - 298*g + 450*b)>>12;
                *vdst++ = ((128<<12) + 450*r - 377*g - 73*b)>>12;
            }

            yplane += 2*ystride;
            uplane += uvstride;
            vplane += uvstride;
        }
    }

    void encodeyuv(const uchar *pixels)
    {
        const int flip = -1;
        const uint planesize = videow * videoh;
        if(!yuv) yuv = new uchar[(planesize*3)/2];
        uchar *yplane = yuv, *uplane = yuv + planesize, *vplane = yuv + planesize + planesize/4;
        const int ystride = flip*int(videow), uvstride = flip*int(videow)/2;
        if(flip < 0) { yplane -= int(videoh-1)*ystride; uplane -= int(videoh/2-1)*uvstride; vplane -= int(videoh/2-1)*uvstride; }

        const uint stride = videow<<2;
        const uchar *src = pixels, *yend = src + videoh*stride;
        while(src < yend)    
        {
            const uchar *src2 = src + stride, *xend = src2;
            uchar *ydst = yplane, *ydst2 = yplane + ystride, *udst = uplane, *vdst = vplane;
            while(src < xend)
            {
                const uint b1 = src[0], g1 = src[1], r1 = src[2],
                           b2 = src[4], g2 = src[5], r2 = src[6],
                           b3 = src2[0], g3 = src2[1], r3 = src2[2],
                           b4 = src2[4], g4 = src2[5], r4 = src2[6];

                // Y  = 16 + 65.481*R + 128.553*G + 24.966*B
                // Cb = 128 - 37.797*R - 74.203*G + 112.0*B
                // Cr = 128 + 112.0*R - 93.786*G - 18.214*B
                *ydst++ = ((16<<12) + 1052*r1 + 2065*g1 + 401*b1)>>12;
                *ydst++ = ((16<<12) + 1052*r2 + 2065*g2 + 401*b2)>>12;
                *ydst2++ = ((16<<12) + 1052*r3 + 2065*g3 + 401*b3)>>12;;
                *ydst2++ = ((16<<12) + 1052*r4 + 2065*g4 + 401*b4)>>12;;

                const uint b = b1 + b2 + b3 + b4,
                           g = g1 + g2 + g3 + g4,
                           r = r1 + r2 + r3 + r4;
                // note: weights here are scaled by 1<<10, as opposed to 1<<12, since r/g/b are already *4
                *udst++ = ((128<<12) - 152*r - 298*g + 450*b)>>12;
                *vdst++ = ((128<<12) + 450*r - 377*g - 73*b)>>12;

                src += 8;
                src2 += 8; 
            }
            src = src2;
            yplane += 2*ystride;
            uplane += uvstride;
            vplane += uvstride;
        }
    }

    void compressyuv(const uchar *pixels)
    {
        const int flip = -1;
        const uint planesize = videow * videoh;
        if(!yuv) yuv = new uchar[(planesize*3)/2];
        uchar *yplane = yuv, *uplane = yuv + planesize, *vplane = yuv + planesize + planesize/4;
        const int ystride = flip*int(videow), uvstride = flip*int(videow)/2;
        if(flip < 0) { yplane -= int(videoh-1)*ystride; uplane -= int(videoh/2-1)*uvstride; vplane -= int(videoh/2-1)*uvstride; }

        const uint stride = videow<<2;
        const uchar *src = pixels, *yend = src + videoh*stride;
        while(src < yend)
        {
            const uchar *src2 = src + stride, *xend = src2;
            uchar *ydst = yplane, *ydst2 = yplane + ystride, *udst = uplane, *vdst = vplane;
            while(src < xend)
            {
                *ydst++ = src[0];
                *ydst++ = src[4];
                *ydst2++ = src2[0];
                *ydst2++ = src2[4];
 
                *udst++ = (uint(src[1]) + uint(src[5]) + uint(src2[1]) + uint(src2[5])) >> 2;
                *vdst++ = (uint(src[2]) + uint(src[6]) + uint(src2[2]) + uint(src2[6])) >> 2;

                src += 8;
                src2 += 8;
            }
            src = src2;
            yplane += 2*ystride;
            uplane += uvstride;
            vplane += uvstride;
        }
    }

    bool writesound(uchar *data, uint framesize, uint frame)
    {
        // do conversion in-place to little endian format
        // note that xoring by half the range yields the same bit pattern as subtracting the range regardless of signedness
        // ... so can toggle signedness just by xoring the high byte with 0x80
        switch(soundformat)
        {
            case AUDIO_U8:
                for(uchar *dst = data, *end = &data[framesize]; dst < end; dst++) *dst ^= 0x80;
                break;
            case AUDIO_S8:
                break;
            case AUDIO_U16LSB:
                for(uchar *dst = &data[1], *end = &data[framesize]; dst < end; dst += 2) *dst ^= 0x80;
                break;
            case AUDIO_U16MSB:
                for(ushort *dst = (ushort *)data, *end = (ushort *)&data[framesize]; dst < end; dst++)
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
                    *dst = endianswap(*dst) ^ 0x0080;
#else
                    *dst = endianswap(*dst) ^ 0x8000;
#endif
                break;
            case AUDIO_S16LSB:
                break;
            case AUDIO_S16MSB:
                endianswap((short *)data, framesize/2);
                break;
        }

        if(pipe) return fwrite(data, 1, framesize, soundf) == framesize;
       
        if(totalsize - segments.last().offset + framesize > 1000*1000*1000 && !nextsegment()) return false;
 
        addindex(frame, 1, framesize);
    
        writechunk("01wb", data, framesize);

        return true;
    }
   

    enum
    {
        VID_RGB = 0,
        VID_YUV,
        VID_YUV420
    };

    void flushsegment()
    {
        endlistchunk(); // LIST movi

        avisegmentinfo &seg = segments.last();

        uint indexframes = 0, videoframes = 0, soundframes = 0;
        for(int i = seg.firstindex; i < index.length(); i++)
        {
            aviindexentry &e = index[i];
            if(e.type) soundframes++; 
            else 
            {
                if(i == seg.firstindex || e.offset != index[i-1].offset)
                    videoframes++;
                indexframes++;
            }
        }

        if(segments.length() == 1)
        {
            startchunk("idx1", index.length()*16);
            loopv(index)
            {
                aviindexentry &entry = index[i];
                // printf("%3d %s %08x\n", i, (entry.type==1)?"s":"v", entry.offset);
                f->write(entry.type ? "01wb" : "00dc", 4); // chunkid
                f->putlil<uint>(0x10); // flags - KEYFRAME
                f->putlil<uint>(entry.offset); // offset (relative to movi)
                f->putlil<uint>(entry.size); // size
            }
            endchunk();
        }

        seg.videoframes = videoframes;
        seg.videoindexoffset = totalsize;
        startchunk("ix00", 24 + indexframes*8);
        f->putlil<ushort>(2); // longs per entry
        f->putlil<ushort>(0x0100); // index of chunks
        f->putlil<uint>(indexframes); // entries in use
        f->write("00dc", 4); // chunk id
        f->putlil<uint>(seg.offset&stream::offset(0xFFFFFFFFU)); // offset low
        f->putlil<uint>(seg.offset>>32); // offset high
        f->putlil<uint>(0); // reserved 3
        for(int i = seg.firstindex; i < index.length(); i++)
        {
            aviindexentry &e = index[i];
            if(e.type) continue;
            f->putlil<uint>(e.offset + 4 + 4);
            f->putlil<uint>(e.size);
        }
        endchunk(); // ix00
        seg.videoindexsize = uint(totalsize - seg.videoindexoffset);

        if(soundframes)
        {
            seg.soundframes = soundframes;
            seg.soundindexoffset = totalsize;
            startchunk("ix01", 24 + soundframes*8);
            f->putlil<ushort>(2); // longs per entry
            f->putlil<ushort>(0x0100); // index of chunks
            f->putlil<uint>(soundframes); // entries in use
            f->write("01wb", 4); // chunk id
            f->putlil<uint>(seg.offset&stream::offset(0xFFFFFFFFU)); // offset low
            f->putlil<uint>(seg.offset>>32); // offset high
            f->putlil<uint>(0); // reserved 3
            for(int i = seg.firstindex; i < index.length(); i++)
            {
                aviindexentry &e = index[i];
                if(!e.type) continue;
                f->putlil<uint>(e.offset + 4 + 4);
                f->putlil<uint>(e.size);
            }
            endchunk(); // ix01
            seg.soundindexsize = uint(totalsize - seg.soundindexoffset);
        }

        endlistchunk(); // RIFF AVI/AVIX
    }

    bool nextsegment()
    {
        if(segments.length()) 
        {
            if(segments.length() >= MAX_SUPER_INDEX) return false;
            flushsegment();
            listchunk("RIFF", "AVIX");
        }
        listchunk("LIST", "movi");
        segments.add(avisegmentinfo(chunkoffsets[chunkdepth], index.length()));
        return true;
    }
  
    bool writevideoframe(const uchar *pixels, uint srcw, uint srch, int format, uint frame)
    {
        if(frame < videoframes) return true;
        
        switch(format)
        {
            case VID_RGB: 
                if(srcw != videow || srch != videoh) scaleyuv(pixels, srcw, srch);
                else encodeyuv(pixels);
                break;
            case VID_YUV:
                compressyuv(pixels);
                break;
        }

        const uint framesize = (videow * videoh * 3) / 2;
        if(pipe)
        {
            const uchar *data = format == VID_YUV420 ? pixels : yuv;
            for(; videoframes <= frame; videoframes++) if(fwrite(data, 1, framesize, pipe) != framesize) return false;
            return true;
        }
        if(totalsize - segments.last().offset + framesize > 1000*1000*1000 && !nextsegment()) return false;

        while(videoframes <= frame) addindex(videoframes++, 0, framesize);

        writechunk("00dc", format == VID_YUV420 ? pixels : yuv, framesize);

        return true;
    }
    
};

VAR(movieaccelblit, 0, 0, 1);
VAR(movieaccelyuv, 0, 1, 1);
VARP(movieaccel, 0, 1, 1);
VARP(moviesync, 0, 0, 1);
FVARP(movieminquality, 0, 0, 1);

namespace recorder
{
    static enum { REC_OK = 0, REC_USERHALT, REC_TOOSLOW, REC_FILERROR } state = REC_OK;
    
    static aviwriter *file = NULL;
    static int starttime = 0;
    
    static int stats[1000];
    static int statsindex = 0;
    static uint dps = 0; // dropped frames per sample
    
    enum { MAXSOUNDBUFFERS = 128 }; // sounds queue up until there is a video frame, so at low fps you'll need a bigger queue
    struct soundbuffer
    {
        uchar *sound;
        uint size, maxsize;
        uint frame;
        
        soundbuffer() : sound(NULL), maxsize(0) {}
        ~soundbuffer() { cleanup(); }
        
        void load(uchar *stream, uint len, uint fnum)
        {
            if(len > maxsize)
            {
                DELETEA(sound);
                sound = new uchar[len];
                maxsize = len;
            }
            size = len;
            frame = fnum;
            memcpy(sound, stream, len);
        }
        
        void cleanup() { DELETEA(sound); maxsize = 0; }
    };
    static queue<soundbuffer, MAXSOUNDBUFFERS> soundbuffers;
    static SDL_mutex *soundlock = NULL;
    
    enum { MAXVIDEOBUFFERS = 2 }; // double buffer
    struct videobuffer 
    {
        uchar *video;
        uint w, h, bpp, frame;
        int format;

        videobuffer() : video(NULL){}
        ~videobuffer() { cleanup(); }

        void init(int nw, int nh, int nbpp)
        {
            DELETEA(video);
            w = nw;
            h = nh;
            bpp = nbpp;
            video = new uchar[w*h*bpp];
            format = -1;
        }
         
        void cleanup() { DELETEA(video); }
    };
    static queue<videobuffer, MAXVIDEOBUFFERS> videobuffers;
    static uint lastframe = ~0U;

    static GLuint scalefb = 0, scaletex[2] = { 0, 0 };
    static uint scalew = 0, scaleh = 0;
    static GLuint encodefb = 0, encoderb = 0;

    static SDL_Thread *thread = NULL;
    static SDL_mutex *videolock = NULL;
    static SDL_cond *shouldencode = NULL, *shouldread = NULL;

    // rendering: every main-loop frame advances the game by one video frame, whatever wall time passed;
    // while held, frames advance the clock but aren't captured
    static bool rendering = false, renderhold = false;
    static uint renderframes = 0, renderclock = 0;
    // while rendering, the game draws at the movie's size into renderfb and the window shows it scaled
    static GLuint renderfb = 0, rendercolor = 0, renderdepth = 0;
    static int renderw = 0, renderh = 0, windoww = 0, windowh = 0;
    static void freerendertarget();

    bool isrecording() { return file != NULL; }
    
    float calcquality()
    {
        return 1.0f - float(dps)/float(dps+file->videofps); // strictly speaking should lock to read dps - 1.0=perfect, 0.5=half of frames are beingdropped
    }

    int gettime()
    {
        return inbetweenframes ? getclockmillis() : totalmillis;
    }

    int videoencoder(void *data) // runs on a separate thread
    {
        for(int numvid = 0, numsound = 0;;)
        {   
            SDL_LockMutex(videolock);
            for(; numvid > 0; numvid--) videobuffers.remove();
            SDL_CondSignal(shouldread);
            while(videobuffers.empty() && state == REC_OK) SDL_CondWait(shouldencode, videolock);
            if(state != REC_OK) { SDL_UnlockMutex(videolock); break; }
            videobuffer &m = videobuffers.removing();
            numvid++;
            SDL_UnlockMutex(videolock);
            
            if(file->soundfrequency > 0)
            {
                // chug data from lock protected buffer to avoid holding lock while writing to file
                SDL_LockMutex(soundlock);
                for(; numsound > 0; numsound--) soundbuffers.remove();
                for(; numsound < soundbuffers.length(); numsound++)
                {
                    soundbuffer &s = soundbuffers.removing(numsound);
                    if(s.frame > m.frame) break; // sync with video
                }
                SDL_UnlockMutex(soundlock);
                loopi(numsound)
                {
                    soundbuffer &s = soundbuffers.removing(i);
                    if(!file->writesound(s.sound, s.size, s.frame)) state = REC_FILERROR;
                }
            }
            
            int duplicates = m.frame - (int)file->videoframes + 1;
            if(duplicates > 0) // determine how many frames have been dropped over the sample window
            {
                dps -= stats[statsindex];
                stats[statsindex] = duplicates-1;
                dps += stats[statsindex];
                statsindex = (statsindex+1)%file->videofps;
            }
            //printf("frame %d->%d (%d dps): sound = %d bytes\n", file->videoframes, nextframenum, dps, m.soundlength);
            if(calcquality() < movieminquality) state = REC_TOOSLOW;
            else if(!file->writevideoframe(m.video, m.w, m.h, m.format, m.frame)) state = REC_FILERROR;
            
            m.frame = ~0U;
        }
        
        return 0;
    }
    
    void soundencoder(void *udata, Uint8 *stream, int len) // callback occurs on a separate thread
    {
        SDL_LockMutex(soundlock);
        if(soundbuffers.full()) 
        {
            if(movieminquality >= 1) state = REC_TOOSLOW;
        }
        else if(state == REC_OK)
        {
            uint nextframe = (max(gettime() - starttime, 0)*file->videofps)/1000;
            soundbuffer &s = soundbuffers.add();
            s.load((uchar *)stream, len, nextframe);
        }
        SDL_UnlockMutex(soundlock);
    }
    
    void start(const char *filename, int videofps, int videow, int videoh, bool sound, bool render = false)
    {
        if(file) return;
       
        useshaderbyname("moviergb");
        useshaderbyname("movieyuv");
        useshaderbyname("moviey");
        useshaderbyname("movieu");
        useshaderbyname("moviev");
 
        int fps, bestdiff, worstdiff;
        getfps(fps, bestdiff, worstdiff);
        if(!render && videofps > fps) conoutf(CON_WARN, "frame rate may be too low to capture at %d fps", videofps);
        
        if(videow%2) videow += 1;
        if(videoh%2) videoh += 1;

        file = new aviwriter(filename, videow, videoh, videofps, sound);
        if(!file->open()) 
        { 
            conoutf(CON_ERROR, "unable to create file %s", filename);
            DELETEP(file);
            return;
        }
        conoutf("movie recording to: %s %dx%d @ %dfps%s", file->filename, file->videow, file->videoh, file->videofps, (file->soundfrequency>0)?" + sound":"");
        
        starttime = gettime();
        rendering = render;
        renderframes = renderclock = 0;
        loopi(file->videofps) stats[i] = 0;
        statsindex = 0;
        dps = 0;
        
        lastframe = ~0U;
        videobuffers.clear();
        loopi(MAXVIDEOBUFFERS)
        {
            uint w = screenw, h = screenw;
            videobuffers.data[i].init(w, h, 4);
            videobuffers.data[i].frame = ~0U;
        }
        
        soundbuffers.clear();
        
        soundlock = SDL_CreateMutex();
        videolock = SDL_CreateMutex();
        shouldencode = SDL_CreateCond();
        shouldread = SDL_CreateCond();
        thread = SDL_CreateThread(videoencoder, "video encoder", NULL); 
        if(file->soundfrequency > 0) Mix_SetPostMix(soundencoder, NULL);
    }
    
    void cleanup()
    {
        if(scalefb) { glDeleteFramebuffers_(1, &scalefb); scalefb = 0; }
        if(scaletex[0] || scaletex[1]) { glDeleteTextures(2, scaletex); memset(scaletex, 0, sizeof(scaletex)); }
        scalew = scaleh = 0;
        if(encodefb) { glDeleteFramebuffers_(1, &encodefb); encodefb = 0; }
        if(encoderb) { glDeleteRenderbuffers_(1, &encoderb); encoderb = 0; }
    }

    void stop()
    {
        if(!file) return;
        if(state == REC_OK) state = REC_USERHALT;
        if(file->soundfrequency > 0) Mix_SetPostMix(NULL, NULL);
        
        SDL_LockMutex(videolock); // wakeup thread enough to kill it
        SDL_CondSignal(shouldencode);
        SDL_UnlockMutex(videolock);
        
        SDL_WaitThread(thread, NULL); // block until thread is finished

        cleanup();

        loopi(MAXVIDEOBUFFERS) videobuffers.data[i].cleanup();
        loopi(MAXSOUNDBUFFERS) soundbuffers.data[i].cleanup();

        SDL_DestroyMutex(soundlock);
        SDL_DestroyMutex(videolock);
        SDL_DestroyCond(shouldencode);
        SDL_DestroyCond(shouldread);

        soundlock = videolock = NULL;
        shouldencode = shouldread = NULL;
        thread = NULL;
 
        static const char * const mesgs[] = { "ok", "stopped", "computer too slow", "file error"};
        conoutf("movie recording halted: %s, %d frames", mesgs[state], file->videoframes);
        if(rendering && file->pipe && state == REC_USERHALT) copystring(lastrender, findfile(file->filename, "wb"));
        else lastrender[0] = '\0';
        if(rendering)
        {
            copystring(lastmovie, findfile(file->filename, "wb"));
            copystring(lastmoviename, file->filename);
        }
        
        DELETEP(file);
        state = REC_OK;
        rendering = false;
        freerendertarget();
    }
 
    void readbuffer(videobuffer &m, uint nextframe)
    {
        bool accelyuv = movieaccelyuv && !(m.w%8),
             usefbo = movieaccel && file->videow <= (uint)screenw && file->videoh <= (uint)screenh && (accelyuv || file->videow < (uint)screenw || file->videoh < (uint)screenh);
        uint w = screenw, h = screenh;
        if(usefbo) { w = file->videow; h = file->videoh; }
        if(w != m.w || h != m.h) m.init(w, h, 4);
        m.format = aviwriter::VID_RGB;
        m.frame = nextframe;

        glPixelStorei(GL_PACK_ALIGNMENT, texalign(m.video, m.w, 4));
        if(usefbo)
        {
            uint tw = screenw, th = screenh;
            if(hasFBB && movieaccelblit) { tw = max(tw/2, m.w); th = max(th/2, m.h); }
            if(tw != scalew || th != scaleh)
            {
                if(!scalefb) glGenFramebuffers_(1, &scalefb);
                loopi(2)
                {
                    if(!scaletex[i]) glGenTextures(1, &scaletex[i]);
                    createtexture(scaletex[i], tw, th, NULL, 3, 1, GL_RGB);
                }
                scalew = tw;
                scaleh = th;
            }
            if(accelyuv && (!encodefb || !encoderb))
            {
                if(!encodefb) glGenFramebuffers_(1, &encodefb);
                glBindFramebuffer_(GL_FRAMEBUFFER, encodefb);
                if(!encoderb) glGenRenderbuffers_(1, &encoderb);
                glBindRenderbuffer_(GL_RENDERBUFFER, encoderb);
                glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA, (m.w*3)/8, m.h);
                glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, encoderb);
                glBindRenderbuffer_(GL_RENDERBUFFER, 0);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            }
                     
            if(tw < (uint)screenw || th < (uint)screenh)
            {
                glBindFramebuffer_(GL_READ_FRAMEBUFFER, screenfbo);
                glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, scalefb);
                glFramebufferTexture2D_(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scaletex[0], 0);
                glBlitFramebuffer_(0, 0, screenw, screenh, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, screenfbo);
            }
            else
            {
                glBindTexture(GL_TEXTURE_2D, scaletex[0]);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenw, screenh);
            }

            GLOBALPARAMF(moviescale, 1.0f/scalew, 1.0f/scaleh);
            if(tw > m.w || th > m.h || (!accelyuv && tw >= m.w && th >= m.h))
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, scalefb);
                do
                {
                    uint dw = max(tw/2, m.w), dh = max(th/2, m.h);
                    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scaletex[1], 0);
                    glViewport(0, 0, dw, dh);
                    glBindTexture(GL_TEXTURE_2D, scaletex[0]);
                    if(dw == m.w && dh == m.h && !accelyuv) { SETSHADER(movieyuv); m.format = aviwriter::VID_YUV; }
                    else SETSHADER(moviergb);
                    screenquad(tw/float(scalew), th/float(scaleh));
                    tw = dw;
                    th = dh;
                    swap(scaletex[0], scaletex[1]);
                } while(tw > m.w || th > m.h);
            }
            if(accelyuv)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, encodefb);
                glBindTexture(GL_TEXTURE_2D, scaletex[0]);
                glViewport(0, 0, m.w/4, m.h); SETSHADER(moviey); screenquadflipped(m.w/float(scalew), m.h/float(scaleh));
                glViewport(m.w/4, 0, m.w/8, m.h/2); SETSHADER(movieu); screenquadflipped(m.w/float(scalew), m.h/float(scaleh));
                glViewport(m.w/4, m.h/2, m.w/8, m.h/2); SETSHADER(moviev); screenquadflipped(m.w/float(scalew), m.h/float(scaleh));
                const uint planesize = m.w * m.h;
                glPixelStorei(GL_PACK_ALIGNMENT, texalign(m.video, m.w/4, 4)); 
                glReadPixels(0, 0, m.w/4, m.h, GL_BGRA, GL_UNSIGNED_BYTE, m.video);
                glPixelStorei(GL_PACK_ALIGNMENT, texalign(&m.video[planesize], m.w/8, 4));
                glReadPixels(m.w/4, 0, m.w/8, m.h/2, GL_BGRA, GL_UNSIGNED_BYTE, &m.video[planesize]);
                glPixelStorei(GL_PACK_ALIGNMENT, texalign(&m.video[planesize + planesize/4], m.w/8, 4));
                glReadPixels(m.w/4, m.h/2, m.w/8, m.h/2, GL_BGRA, GL_UNSIGNED_BYTE, &m.video[planesize + planesize/4]);
                m.format = aviwriter::VID_YUV420;
            }
            else
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, scalefb);
                glReadPixels(0, 0, m.w, m.h, GL_BGRA, GL_UNSIGNED_BYTE, m.video);
            }
            glBindFramebuffer_(GL_FRAMEBUFFER, screenfbo);
            glViewport(0, 0, screenw, screenh);

        }
        else glReadPixels(0, 0, m.w, m.h, GL_BGRA, GL_UNSIGNED_BYTE, m.video);
    }
 
    bool readbuffer()
    {
        if(!file) return false;
        if(state != REC_OK)
        {
            stop();
            return false;
        }
        if(rendering && renderhold) return true;
        SDL_LockMutex(videolock);
        if(rendering) while(videobuffers.full() && state == REC_OK) SDL_CondWait(shouldread, videolock);
        else if(moviesync && videobuffers.full()) SDL_CondWait(shouldread, videolock);
        uint nextframe = rendering ? renderframes++ : (max(gettime() - starttime, 0)*file->videofps)/1000;
        if(!videobuffers.full() && (lastframe == ~0U || nextframe > lastframe))
        {
            videobuffer &m = videobuffers.adding();
            SDL_UnlockMutex(videolock);
            readbuffer(m, nextframe);
            SDL_LockMutex(videolock);
            lastframe = nextframe;
            videobuffers.add();
            SDL_CondSignal(shouldencode);
        }
        SDL_UnlockMutex(videolock);
        return true;
    }

    void drawhud()
    {
        int w = screenw, h = screenh;
        if(forceaspect) w = int(ceil(h*forceaspect));
        gettextres(w, h);

        hudmatrix.ortho(0, w, h, 0, -1, 1);
        hudmatrix.scale(1/3.0f, 1/3.0f, 1);
        resethudmatrix();
        hudshader->set();

        glEnable(GL_BLEND);

        double totalsize = file->filespaceguess();
        const char *unit = "KB";
        if(totalsize >= 1e9) { totalsize /= 1e9; unit = "GB"; }
        else if(totalsize >= 1e6) { totalsize /= 1e6; unit = "MB"; }
        else totalsize /= 1e3;

        draw_textf("recorded %.1f%s %d%%", w*3-10*FONTH, h*3-FONTH-FONTH*3/2, totalsize, unit, int(calcquality()*100)); 

        glDisable(GL_BLEND);
    }

    // shown in the window while a render runs, never in the movie
    static string progresslabel = "", progresshint = "";
    static float progressfrac = 0;

    // what the movie being rendered takes on disk so far
    static double moviebytes()
    {
        if(file) return file->filespaceguess();
        double bytes = 0;
        struct stat st;
        if(lastmovie[0] && stat(lastmovie, &st) == 0) bytes += st.st_size;
        if(soundpassfile) bytes += ftell(soundpassfile);
        return bytes;
    }

    static void drawprogress(int w, int h)
    {
        if(!progresslabel[0]) return;
        hudmatrix.ortho(0, w, h, 0, -1, 1);
        resethudmatrix();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        float bw = w*0.4f, bh = max(h/120.0f, 4.0f), bx = (w - bw)/2, by = h*0.86f, fill = bw*clamp(progressfrac, 0.0f, 1.0f);
        if(progressfrac >= 0)
        {
            hudnotextureshader->set();
            gle::defvertex(2);
            gle::colorf(0, 0, 0, 0.6f);
            gle::begin(GL_TRIANGLE_STRIP);
            gle::attribf(bx, by); gle::attribf(bx + bw, by); gle::attribf(bx, by + bh); gle::attribf(bx + bw, by + bh);
            gle::end();
            gle::colorf(1, 1, 1, 0.9f);
            gle::begin(GL_TRIANGLE_STRIP);
            gle::attribf(bx, by); gle::attribf(bx + fill, by); gle::attribf(bx, by + bh); gle::attribf(bx + fill, by + bh);
            gle::end();
        }
        string text;
        if(progressfrac >= 0) formatstring(text, "%s %d%%   %.1f MB", progresslabel, int(min(progressfrac, 1.0f)*100), moviebytes()/1e6);
        else formatstring(text, "%s   %.1f MB", progresslabel, moviebytes()/1e6);
        float scale = h/(FONTH*36.0f);
        pushhudmatrix();
        hudmatrix.scale(scale, scale, 1);
        flushhudmatrix();
        hudshader->set();
        gle::colorf(1, 1, 1);
        int tw = 0, th = 0;
        text_bounds(text, tw, th);
        draw_text(text, int((w/scale - tw)/2), int(by/scale) - th - th/4);
        if(progresshint[0])
        {
            int hw = 0, hh = 0;
            text_bounds(progresshint, hw, hh);
            draw_text(progresshint, int((w/scale - hw)/2), int((by + bh)/scale) + hh/4, 255, 255, 255, 170);
        }
        pophudmatrix();
        glDisable(GL_BLEND);
    }

    static void drawpreview()
    {
        float scale = min(windoww/float(renderw), windowh/float(renderh));
        int pw = int(renderw*scale), ph = int(renderh*scale), px = (windoww - pw)/2, py = (windowh - ph)/2;
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, windoww, windowh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer_(GL_READ_FRAMEBUFFER, renderfb);
        glBlitFramebuffer_(0, 0, renderw, renderh, px, py, px + pw, py + ph, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        drawprogress(windoww, windowh);
        glClearColor(0, 0, 0, 0);
    }

    void capture(bool overlay)
    {
        // loading screens (swapped without overlay) are shown in the window but not put in a rendered movie
        if(rendering && !overlay) return;
        if(readbuffer() && overlay && !rendering) drawhud();
        if(rendering && renderfb) drawpreview();
        else if(!rendering) drawprogress(screenw, screenh);
    }

    static void freerendertarget()
    {
        if(!renderfb) return;
        screenw = windoww;
        screenh = windowh;
        screenfbo = 0;
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers_(1, &renderfb);
        glDeleteRenderbuffers_(1, &rendercolor);
        glDeleteRenderbuffers_(1, &renderdepth);
        renderfb = rendercolor = renderdepth = 0;
        gl_resize();
    }

    static bool makerendertarget(int w, int h)
    {
        GLint maxsize = 0;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxsize);
        if(!hasFBB || w > maxsize || h > maxsize) return false;
        glGenFramebuffers_(1, &renderfb);
        glGenRenderbuffers_(1, &rendercolor);
        glGenRenderbuffers_(1, &renderdepth);
        glBindFramebuffer_(GL_FRAMEBUFFER, renderfb);
        glBindRenderbuffer_(GL_RENDERBUFFER, rendercolor);
        glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rendercolor);
        glBindRenderbuffer_(GL_RENDERBUFFER, renderdepth);
        glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderdepth);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderdepth);
        glBindRenderbuffer_(GL_RENDERBUFFER, 0);
        windoww = screenw;
        windowh = screenh;
        renderw = screenw = w;
        renderh = screenh = h;
        screenfbo = renderfb;
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { freerendertarget(); return false; }
        gl_resize();
        return true;
    }

    int renderstep()
    {
        if(!file || !rendering) return 0;
        // the window's framebuffer stays bound through the swap, or macOS shows nothing
        if(renderfb)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, renderfb);
            glViewport(0, 0, renderw, renderh);
        }
        if(renderfb && (screenw != renderw || screenh != renderh))
        {
            windoww = screenw;
            windowh = screenh;
            screenw = renderw;
            screenh = renderh;
            gl_resize();
        }
        uint n = renderclock++, fps = file->videofps;
        return int(((n+1)*1000ULL)/fps - (n*1000ULL)/fps);
    }
}

GLuint screenfbo = 0;

bool movierender(const char *name, int fps, int w, int h, bool hold)
{
    if(recorder::isrecording()) return false;
    if(w <= 0 || h <= 0) { w = screenw; h = screenh; }
    w = (w + 1)&~1;
    h = (h + 1)&~1;
    if(!recorder::makerendertarget(w, h)) { conoutf(CON_ERROR, "can't render at %dx%d", w, h); return false; }
    recorder::renderhold = hold;
    recorder::start(name, fps, w, h, false, true);
    if(!recorder::isrecording()) recorder::freerendertarget();
    return recorder::isrecording();
}

void movierenderstop() { if(recorder::rendering) recorder::stop(); }

void moviehudres(int &w, int &h)
{
    if(!recorder::renderfb || recorder::windowh <= 0 || h <= 0) return;
    w = int(w*(long long)recorder::windowh/h);
    h = recorder::windowh;
}
bool movierendering() { return recorder::rendering; }
void movierenderhold(bool on) { recorder::renderhold = on; }

void movieprogress(const char *label, float frac, const char *hint)
{
    copystring(recorder::progresslabel, label ? label : "");
    copystring(recorder::progresshint, hint ? hint : "");
    recorder::progressfrac = frac;
}

const char *movielastname() { return lastmoviename; }

void moviediscard()
{
    if(lastmovie[0]) remove(lastmovie);
    lastmovie[0] = lastmoviename[0] = lastrender[0] = '\0';
}
int movierenderstep() { return recorder::renderstep(); }
uint movierenderframes() { return recorder::rendering ? recorder::renderframes : 0; }

VARP(moview, 0, 320, 10000);
VARP(movieh, 0, 240, 10000);
VARP(moviefps, 1, 24, 1000);
VARP(moviesound, 0, 1, 1);

void movie(char *name)
{
    if(name[0] == '\0') recorder::stop();
    else if(!recorder::isrecording()) recorder::start(name, moviefps, moview ? moview : screenw, movieh ? movieh : screenh, moviesound!=0);
}

COMMAND(movie, "s");

// The sound of a rendered movie is recorded in a second, real-time pass, as the mixer plays it.
static SDL_atomic_t soundpassbytes, soundpassmarkbytes;
static string soundpassname = "";
static int soundpassfreq = 0, soundpasschannels = 0;
static const char *soundpassformat = NULL;

static void soundpassmix(void *udata, Uint8 *stream, int len)
{
    if(!soundpassfile) return;
    SDL_AtomicCAS(&soundpassmarkbytes, -2, SDL_AtomicGet(&soundpassbytes));
    if(fwrite(stream, 1, len, soundpassfile) == size_t(len)) SDL_AtomicAdd(&soundpassbytes, len);
}

bool moviesoundstart()
{
    extern bool nosound;
    if(!lastrender[0] || nosound || !moviesound || soundpassfile) return false;
    Uint16 format = 0;
    if(!Mix_QuerySpec(&soundpassfreq, &format, &soundpasschannels)) return false;
    switch(format)
    {
        case AUDIO_U8: soundpassformat = "u8"; break;
        case AUDIO_S8: soundpassformat = "s8"; break;
        case AUDIO_U16LSB: soundpassformat = "u16le"; break;
        case AUDIO_U16MSB: soundpassformat = "u16be"; break;
        case AUDIO_S16LSB: soundpassformat = "s16le"; break;
        case AUDIO_S16MSB: soundpassformat = "s16be"; break;
        case AUDIO_F32LSB: soundpassformat = "f32le"; break;
        default: return false;
    }
    formatstring(soundpassname, "%s.sound", lastrender);
    soundpassfile = fopen(soundpassname, "wb");
    if(!soundpassfile) return false;
    SDL_AtomicSet(&soundpassbytes, 0);
    SDL_AtomicSet(&soundpassmarkbytes, -1);
    Mix_SetPostMix(soundpassmix, NULL);
    return true;
}

bool moviesoundrecording() { return soundpassfile != NULL; }

void moviesoundmark() { if(soundpassfile) SDL_AtomicSet(&soundpassmarkbytes, -2); }

int moviesoundmarkms()
{
    int bytes = SDL_AtomicGet(&soundpassmarkbytes);
    if(!soundpassfile || bytes < 0) return -1;
    int samplesize = soundpasschannels * (!strcmp(soundpassformat, "f32le") ? 4 : soundpassformat[1] == '8' ? 1 : 2);
    return int(bytes * 1000LL / (samplesize * soundpassfreq));
}

// mux: add what was recorded to the movie, padded or cut to its length; else drop it. The movie is put
// together from n pieces of the recording, piece i starting at startms[i] in it and lasting lengthms[i]
// (the last: to the end); a negative start begins that piece with silence.
void moviesoundstop(bool mux, const int *startms, const int *lengthms, int n)
{
    if(!soundpassfile) return;
    Mix_SetPostMix(NULL, NULL);
    bool hassound = ftell(soundpassfile) > 0;
    fclose(soundpassfile);
    soundpassfile = NULL;
    if(mux && hassound)
    {
        string tmpname;
        formatstring(tmpname, "%s.tmp.mp4", lastrender);
        cmdline video, sound, out, args;
        quotearg(video, lastrender);
        quotearg(sound, soundpassname);
        quotearg(out, tmpname);
        cmdline filter;
        formatstring(filter, "[1:a]asplit=%d", n);
        loopi(n) concformatstring(filter, "[in%d]", i);
        loopi(n)
        {
            int start = startms[i];
            concformatstring(filter, ";[in%d]", i);
            if(start >= 0) concformatstring(filter, "atrim=start=%d.%03d", start/1000, start%1000);
            else concformatstring(filter, "adelay=delays=%d:all=1", -start);
            if(i < n-1) concformatstring(filter, ",atrim=duration=%d.%03d", lengthms[i]/1000, lengthms[i]%1000);
            concformatstring(filter, ",asetpts=PTS-STARTPTS[piece%d]", i);
        }
        concatstring(filter, ";");
        loopi(n) concformatstring(filter, "[piece%d]", i);
        concformatstring(filter, "concat=n=%d:v=0:a=1,apad[sound]", n);
        cmdline graph, mapsound;
        quotearg(graph, filter);
        quotearg(mapsound, "[sound]");
        formatstring(args, "-y -loglevel error -i %s -f %s -ar %d -ac %d -i %s -filter_complex %s -map 0:v -map %s -c:v copy -c:a aac -b:a 192k -shortest -movflags +faststart %s",
            video, soundpassformat, soundpassfreq, soundpasschannels, sound, graph, mapsound, out);
        bool muxed = runffmpeg(args, "r") == 0;
        if(muxed) remove(lastrender);
        if(muxed && !rename(tmpname, lastrender)) { if(dbgmovie) conoutf(CON_DEBUG, "movie with sound: %s", lastrender); }
        else { conoutf(CON_ERROR, "could not add the sound to %s", lastrender); remove(tmpname); }
    }
    remove(soundpassname);
    lastrender[0] = '\0';
}
ICOMMAND(movierecording, "", (), intret(recorder::isrecording() ? 1 : 0));

