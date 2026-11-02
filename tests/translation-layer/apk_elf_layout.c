/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Static analysis using Husk's production ELF/page planner, not a load/launch. */
#include "husk-tl-internal.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    if(argc<2) return 2;
    for(int i=1;i<argc;i++) {
        FILE *f=fopen(argv[i],"rb"); if(!f) return 2;
        if(fseek(f,0,SEEK_END)) return 2;
        long n=ftell(f); rewind(f); if(n<=0) return 2;
        unsigned char *data=malloc((size_t)n); if(!data) return 2;
        if(fread(data,1,(size_t)n,f)!=(size_t)n) return 2;
        fclose(f);
        tl_elf_report r; tl_elf_analyze(data,(size_t)n,&r); free(data);
        printf("%s: ok=%d machine=%u span=%llu pages=%u mixed=%u unsupported-relocs=%u layout=%s\n",
               argv[i],r.ok,r.machine,(unsigned long long)r.span,r.pages,r.pages_conflict,r.relocs_unsupported,r.layout);
        if(!r.ok || !r.is64 || r.machine!=183 || !r.pages || r.relocs_unsupported) {
            fprintf(stderr,"FAIL: %s\n",r.error); return 1;
        }
    }
    puts("PASS: APK ARM64 ELF layouts and relocation types inspected with Husk's production analyzer");
    return 0;
}
