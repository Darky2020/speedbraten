// The keyboards, mice and other generic desktop input devices the system reports, for replay
// files: kind, USB vendor and product ID, transport and product name, never serial numbers.
// System libraries are loaded at run time, so the client links nothing extra.

#include "engine.h"

#if defined(HEADLESS)

void listinputdevices(vector<inputdevice> &out) {}

#elif defined(WIN32)

static int hexafter(const char *s, const char *tag)
{
    const char *p = strstr(s, tag);
    return p ? int(strtol(p + strlen(tag), NULL, 16)) : 0;
}

void listinputdevices(vector<inputdevice> &out)
{
    typedef UINT (WINAPI *listfn)(PRAWINPUTDEVICELIST, PUINT, UINT);
    typedef UINT (WINAPI *infofn)(HANDLE, UINT, LPVOID, PUINT);
    typedef BOOLEAN (__stdcall *productfn)(HANDLE, PVOID, ULONG);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if(!user32) return;
    listfn list = (listfn)GetProcAddress(user32, "GetRawInputDeviceList");
    infofn info = (infofn)GetProcAddress(user32, "GetRawInputDeviceInfoA");
    HMODULE hid = LoadLibraryA("hid.dll");
    productfn product = hid ? (productfn)GetProcAddress(hid, "HidD_GetProductString") : NULL;
    UINT n = 0;
    if(!list || !info || list(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return;
    vector<RAWINPUTDEVICELIST> devs;
    devs.pad(int(n));
    n = list(devs.getbuf(), &n, sizeof(RAWINPUTDEVICELIST));
    if(n == UINT(-1)) return;
    loopi(int(n))
    {
        char path[1024];
        UINT len = sizeof(path);
        if(info(devs.getbuf()[i].hDevice, RIDI_DEVICENAME, path, &len) == UINT(-1)) continue;
        path[sizeof(path)-1] = '\0';
        inputdevice &d = out.add();
        d.kind = devs.getbuf()[i].dwType == RIM_TYPEKEYBOARD ? 1 : devs.getbuf()[i].dwType == RIM_TYPEMOUSE ? 2 : 0;
        d.vendor = hexafter(path, "VID_");
        d.product = hexafter(path, "PID_");
        // the device path starts with the bus driver, e.g. \\?\HID#..., \\?\ACPI#...
        const char *bus = path[0] == '\\' ? path + 4 : path;
        int buslen = 0;
        while(bus[buslen] && bus[buslen] != '#' && buslen < 30) buslen++;
        copystring(d.transport, bus, buslen + 1);
        d.name[0] = '\0';
        if(product)
        {
            HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if(h != INVALID_HANDLE_VALUE)
            {
                WCHAR wname[128];
                if(product(h, wname, sizeof(wname))) WideCharToMultiByte(CP_UTF8, 0, wname, -1, d.name, sizeof(d.name), NULL, NULL);
                CloseHandle(h);
            }
        }
    }
}

#elif defined(__APPLE__)

#include <dlfcn.h>

void listinputdevices(vector<inputdevice> &out)
{
    typedef const void *ref;
    static void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    static void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    if(!iokit || !cf) return;
    ref (*create)(ref, uint) = (ref (*)(ref, uint))dlsym(iokit, "IOHIDManagerCreate");
    void (*match)(ref, ref) = (void (*)(ref, ref))dlsym(iokit, "IOHIDManagerSetDeviceMatching");
    ref (*copydevices)(ref) = (ref (*)(ref))dlsym(iokit, "IOHIDManagerCopyDevices");
    ref (*property)(ref, ref) = (ref (*)(ref, ref))dlsym(iokit, "IOHIDDeviceGetProperty");
    long (*setcount)(ref) = (long (*)(ref))dlsym(cf, "CFSetGetCount");
    void (*setvalues)(ref, ref *) = (void (*)(ref, ref *))dlsym(cf, "CFSetGetValues");
    ref (*makestring)(ref, const char *, uint) = (ref (*)(ref, const char *, uint))dlsym(cf, "CFStringCreateWithCString");
    bool (*getstring)(ref, char *, long, uint) = (bool (*)(ref, char *, long, uint))dlsym(cf, "CFStringGetCString");
    bool (*getnumber)(ref, long, void *) = (bool (*)(ref, long, void *))dlsym(cf, "CFNumberGetValue");
    unsigned long (*gettype)(ref) = (unsigned long (*)(ref))dlsym(cf, "CFGetTypeID");
    unsigned long (*numbertype)() = (unsigned long (*)())dlsym(cf, "CFNumberGetTypeID");
    unsigned long (*stringtype)() = (unsigned long (*)())dlsym(cf, "CFStringGetTypeID");
    void (*release)(ref) = (void (*)(ref))dlsym(cf, "CFRelease");
    if(!create || !match || !copydevices || !property || !setcount || !setvalues || !makestring || !getstring ||
       !getnumber || !gettype || !numbertype || !stringtype || !release) return;
    const uint UTF8 = 0x08000100;
    const long SINT32 = 3;
    enum { VENDOR = 0, PRODUCTID, PRODUCT, TRANSPORT, PAGE, USAGE, NUMKEYS };
    static const char * const names[NUMKEYS] = { "VendorID", "ProductID", "Product", "Transport", "PrimaryUsagePage", "PrimaryUsage" };
    ref keys[NUMKEYS];
    loopi(NUMKEYS) keys[i] = makestring(NULL, names[i], UTF8);
    ref manager = create(NULL, 0);
    if(manager)
    {
        match(manager, NULL);
        ref set = copydevices(manager);
        if(set)
        {
            long n = setcount(set);
            vector<ref> devs;
            devs.pad(int(n));
            setvalues(set, devs.getbuf());
            loopi(int(n))
            {
                int num[NUMKEYS] = { 0 };
                string str[NUMKEYS];
                loopj(NUMKEYS)
                {
                    str[j][0] = '\0';
                    ref v = property(devs.getbuf()[i], keys[j]);
                    if(!v) continue;
                    if(gettype(v) == numbertype()) getnumber(v, SINT32, &num[j]);
                    else if(gettype(v) == stringtype()) getstring(v, str[j], sizeof(str[j]), UTF8);
                }
                if(num[PAGE] != 1) continue; // generic desktop: keyboards, mice, pointers, game controllers
                inputdevice &d = out.add();
                d.kind = num[USAGE] == 6 || num[USAGE] == 7 ? 1 : num[USAGE] == 2 || num[USAGE] == 1 ? 2 : 0;
                d.vendor = num[VENDOR];
                d.product = num[PRODUCTID];
                copystring(d.transport, str[TRANSPORT]);
                copystring(d.name, str[PRODUCT]);
            }
            release(set);
        }
        release(manager);
    }
    loopi(NUMKEYS) if(keys[i]) release(keys[i]);
}

#else

void listinputdevices(vector<inputdevice> &out)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if(!f) return;
    char line[512];
    inputdevice cur;
    bool have = false;
    int bus = 0;
    while(fgets(line, sizeof(line), f))
    {
        if(line[0] == 'I' && line[1] == ':')
        {
            have = true;
            cur.kind = 0;
            cur.name[0] = '\0';
            const char *b = strstr(line, "Bus="), *v = strstr(line, "Vendor="), *p = strstr(line, "Product=");
            bus = b ? int(strtol(b + 4, NULL, 16)) : 0;
            cur.vendor = v ? int(strtol(v + 7, NULL, 16)) : 0;
            cur.product = p ? int(strtol(p + 8, NULL, 16)) : 0;
            const char *busname = bus == 0x03 ? "USB" : bus == 0x05 ? "Bluetooth" : bus == 0x11 ? "i8042" : bus == 0x06 ? "virtual" : bus == 0x19 ? "host" : "";
            if(busname[0]) copystring(cur.transport, busname);
            else formatstring(cur.transport, "bus %d", bus);
        }
        else if(have && line[0] == 'N' && line[1] == ':')
        {
            const char *q = strchr(line, '"');
            if(q)
            {
                copystring(cur.name, q + 1);
                char *end = strrchr(cur.name, '"');
                if(end) *end = '\0';
            }
        }
        else if(have && line[0] == 'H' && line[1] == ':')
        {
            if(strstr(line, "kbd")) cur.kind = 1;
            if(strstr(line, "mouse")) cur.kind = 2;
        }
        else if(have && (line[0] == '\n' || line[0] == '\0'))
        {
            if(cur.kind) out.add(cur);
            have = false;
        }
    }
    if(have && cur.kind) out.add(cur);
    fclose(f);
}

#endif
