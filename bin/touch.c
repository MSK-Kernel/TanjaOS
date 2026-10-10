#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"
static int next_arg(char **pp,char*out,int max){char*p=*pp;int n=0,q=0;char qc=0;while(*p==' '||*p=='\t')p++;if(!*p){*pp=p;return 0;}while(*p){if(!q&&(*p==' '||*p=='\t'))break;if((*p=='"'||*p=='\'')&&(!q||*p==qc)){if(!q){q=1;qc=*p;}else q=0;p++;continue;}if(*p=='\\'&&p[1]){if(n<max-1)out[n++]=p[1];p+=2;continue;}if(n<max-1)out[n++]=*p;p++;}out[n]=0;*pp=p;return n>0;}
void main(char*args){int no_create=0,any=0;char a[256],*p=args;while(next_arg(&p,a,sizeof(a))){if(a[0]=='-'&&a[1]){int j;for(j=1;a[j];j++){if(a[j]=='c')no_create=1;else if(a[j]=='a'||a[j]=='m'||a[j]=='f'){}else{print("touch: invalid option -");putc(a[j]);print("\n");return;}}continue;}if(!strcmp(a,"--no-create")){no_create=1;continue;}any=1;if(fs_file_exists(a))continue;if(fs_directory_exists(a)){print("touch: cannot touch '");print(a);print("': Is a directory\n");continue;}if(!no_create&&fs_create_file(a)!=0){print("touch: cannot touch '");print(a);print("': Error\n");}}if(!any)print("Usage: touch [-camf] <file>...\n");}
