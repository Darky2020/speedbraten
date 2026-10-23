/* Linked into Linux builds against an older glibc made with newer glibc headers (zig): in C++
   those headers turn strtol and friends into glibc 2.38's C23 variants, which older glibc
   doesn't have. These forward to the classic functions under their own symbol names. */
long glibc_strtol(const char *s, char **end, int base) __asm__("strtol");
long long glibc_strtoll(const char *s, char **end, int base) __asm__("strtoll");
unsigned long glibc_strtoul(const char *s, char **end, int base) __asm__("strtoul");
unsigned long long glibc_strtoull(const char *s, char **end, int base) __asm__("strtoull");

long __isoc23_strtol(const char *s, char **end, int base) { return glibc_strtol(s, end, base); }
long long __isoc23_strtoll(const char *s, char **end, int base) { return glibc_strtoll(s, end, base); }
unsigned long __isoc23_strtoul(const char *s, char **end, int base) { return glibc_strtoul(s, end, base); }
unsigned long long __isoc23_strtoull(const char *s, char **end, int base) { return glibc_strtoull(s, end, base); }
