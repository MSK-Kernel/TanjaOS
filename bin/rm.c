#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

static int next_arg(char **pp, char *out, int max) {
    char *p=*pp; int n=0, quote=0; char q=0;
    while (*p==' '||*p=='\t') p++; if(!*p){*pp=p;return 0;}
    while(*p){
        if(!quote&&(*p==' '||*p=='\t'))break;
        if((*p=='"'||*p=='\'')&&(!quote||*p==q)){if(!quote){quote=1;q=*p;}else quote=0;p++;continue;}
        if(*p=='\\'&&p[1]){if(n<max-1)out[n++]=p[1];p+=2;continue;}
        if(n<max-1)out[n++]=*p; p++;
    }
    out[n]=0;*pp=p;return n>0;
}
void main(char* args){
    int recursive=0, force=0, interactive=0, any=0; char arg[256],*p=args;
    while(next_arg(&p,arg,sizeof(arg))){
        if(arg[0]=='-'&&arg[1]&&arg[1]!='-'){
            int j; for(j=1;arg[j];j++){if(arg[j]=='r'||arg[j]=='R')recursive=1;else if(arg[j]=='f')force=1;else if(arg[j]=='i'||arg[j]=='I')interactive=1;else if(arg[j]=='d'){}else{print("rm: invalid option -");putc(arg[j]);print("\n");return;}} continue;
        }
        if(!strcmp(arg,"--recursive")){recursive=1;continue;} if(!strcmp(arg,"--force")){force=1;continue;} if(!strcmp(arg,"--interactive")){interactive=1;continue;}
        any=1;
        if(fs_file_exists(arg)){ if(interactive){print("rm: remove '");print(arg);print("'? "); int c=get_key(); putc(c); putc('\n'); if(c!='y'&&c!='Y') continue;} fs_delete_file(arg); continue; }
        if(fs_directory_exists(arg)){ if(!recursive){print("rm: cannot remove '");print(arg);print("': Is a directory\n");continue;} if(interactive){print("rm: remove directory '");print(arg);print("'? ");int c=get_key();putc(c);putc('\n');if(c!='y'&&c!='Y')continue;} if(fs_delete_directory_recursive(arg)!=0&&!force){print("rm: cannot remove '");print(arg);print("'\n");} continue; }
        if(!force){print("rm: cannot remove '");print(arg);print("': No such file or directory\n");}
    }
    if(!any&&!force)print("Usage: rm [-firR] <file>...\n");
}
