/* SPDX-License-Identifier: MPL-2.0 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    int (*external)(void) = (int (*)(void))dlsym(library, "original_entry");
    int (*plt)(void) = (int (*)(void))dlsym(library, "call_internal_plt");
    int (*direct)(void) = (int (*)(void))dlsym(library, "call_internal_direct");
    if (!external || !plt || !direct) return 3;
    int a=external(), b=plt(), c=direct(), expected=atoi(argv[2]);
    printf("GNU ELF own fixture external=%d internal_plt=%d internal_direct=%d expected=%d\n", a,b,c,expected);
    if (a != expected || b != expected || c != 11) return 4;
    return dlclose(library) != 0;
}
