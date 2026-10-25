#include "log_sink.h"
#include "timer_policy.h"
#include "power_policy.h"
#include <assert.h>
#include <stdlib.h>
int main(int argc,char **argv) {
 assert(argc==2);
 assert(nfs4_timer_wait_ms(1000,UINT64_MAX)==UINT32_MAX);
 assert(nfs4_timer_wait_ms(1000,1001)==1);
 assert(nfs4_timer_wait_ms(1000,2000)==1);
 assert(nfs4_timer_wait_ms(1000,2001)==2);
 assert(nfs4_timer_wait_ms(1000,1000)==1);
 assert(nfs4_timer_wait_ms(1000,1000001000)==1000000);
 assert(nfs4_scene_cap(60,1)==30 && nfs4_scene_cap(60,0)==60);
 assert(nfs4_scene_cap(30,1)==30 && nfs4_scene_cap(15,1)==15 && nfs4_scene_cap(5,1)==5);
 Nfs4LogSink log={0};char previous[1200];
 snprintf(log.current,sizeof log.current,"%s/log-budget-current.log",argv[1]);
 snprintf(log.segment,sizeof log.segment,"%s/log-budget-segment.log",argv[1]);
 snprintf(previous,sizeof previous,"%s/log-budget-previous.log",argv[1]);
 FILE *f=fopen(previous,"w");assert(f);fputs("previous launch",f);fclose(f);
 log.file=fopen(log.current,"w");assert(log.file);
 for(int i=0;i<1000;i++){char line[80];snprintf(line,sizeof line,"diagnostic %04d complete",i);assert(nfs4_log_append(&log,line,128)==1);assert(log.bytes<=128);}
 assert(nfs4_log_append(&log,"FATAL: test retained",128)==1);fclose(log.file);
 f=fopen(log.current,"r");assert(f);char text[200]={0};fread(text,1,199,f);assert(strstr(text,"FATAL: test retained"));fclose(f);
 f=fopen(log.segment,"r");assert(f);fseek(f,0,SEEK_END);assert(ftell(f)<=128);fclose(f);
 f=fopen(previous,"r");assert(f);memset(text,0,sizeof text);fread(text,1,199,f);assert(!strcmp(text,"previous launch"));fclose(f);
 puts("idle_policy: no-timer blocking, rounded deadlines, menu limits, 1000-line bounded rotation and previous-launch preservation passed");return 0;
}