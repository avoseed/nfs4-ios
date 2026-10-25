/* Caller serializes writes. Keep the preceding launch log separate from the
 * bounded current-session segment. Normal writes stay flushed for diagnostics. */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct { FILE *file; size_t bytes; char current[1200], segment[1200]; } Nfs4LogSink;
static inline int nfs4_log_append(Nfs4LogSink *s,const char *line,size_t limit) {
    if(!s->file)return 0;
    size_t n=strlen(line)+1;
    if(s->bytes+n>limit) {
        fclose(s->file);s->file=NULL;
        remove(s->segment);
        if(rename(s->current,s->segment)!=0)return -1;
        s->file=fopen(s->current,"w");s->bytes=0;
        if(!s->file)return -1;
        int written=fprintf(s->file,"log: earlier lines in nfs4host.segment.log\n");
        if(written>0)s->bytes+=(size_t)written;
    }
    int written=fprintf(s->file,"%s\n",line);
    if(written>0)s->bytes+=(size_t)written;
    fflush(s->file);
    return written<0?-1:1;
}
