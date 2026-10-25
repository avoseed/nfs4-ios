/* Minimal reproducer: x86 codegen drops (x & ~0xFF) when merging a loaded byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *M;
struct S { unsigned a, b, c; };

__attribute__((noinline)) void f(struct S *s) {
    unsigned x = s->b;
    unsigned y = (x & 0xFFFFFF00u) | M[x];
    unsigned z = s->c - 4;
    unsigned v = 5;
    memcpy(M + z, &v, 4);
    s->b = y;
}

int main(void) {
    M = calloc(1, 0x11000000);
    M[0x1004E3D4] = 0xDD;
    struct S s = { 0, 0x1004E3D4, 0x10070000 };
    f(&s);
    printf("%08X (expect 1004E3DD)\n", s.b);
    return s.b != 0x1004E3DD;
}
