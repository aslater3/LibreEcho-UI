#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char*find_key(const char*s,const char*k){char key[96];snprintf(key,sizeof(key),"\"%s\"",k);s=strstr(s,key);if(!s)return 0;s+=strlen(key);while(isspace((unsigned char)*s))s++;if(*s++!=':')return 0;while(isspace((unsigned char)*s))s++;return s;}
int json_valid_object(const char*s,size_t n){int depth=0,str=0,esc=0;size_t i;if(!s||n<2)return 0;while(n&&isspace((unsigned char)*s)){s++;n--;}if(!n||*s!='{')return 0;for(i=0;i<n;i++){char c=s[i];if(str){if(esc)esc=0;else if(c=='\\')esc=1;else if(c=='\"')str=0;}else if(c=='\"')str=1;else if(c=='{'||c=='[')depth++;else if(c=='}'||c==']'){if(--depth<0)return 0;}}while(n&&isspace((unsigned char)s[n-1]))n--;return !str&&depth==0&&n&&s[n-1]=='}';}
int json_get_int(const char*s,const char*k,int*out){char*e;long v;const char*p=find_key(s,k);if(!p)return 0;v=strtol(p,&e,10);if(e==p)return-1;*out=(int)v;return 1;}
int json_get_bool(const char*s,const char*k,int*out){const char*p=find_key(s,k);if(!p)return 0;if(!strncmp(p,"true",4)){*out=1;return 1;}if(!strncmp(p,"false",5)){*out=0;return 1;}return-1;}
int json_get_string(const char*s,const char*k,char*out,size_t z){const char*p=find_key(s,k);size_t i=0;if(!p)return 0;if(*p++!='\"')return-1;while(*p&&*p!='\"'){unsigned char c=(unsigned char)*p++;if(c=='\\'){c=(unsigned char)*p++;if(c=='n')c='\n';else if(c!='\"'&&c!='\\'&&c!='/')return-1;}if(c<32||i+1>=z)return-1;out[i++]=(char)c;}if(*p!='\"')return-1;out[i]=0;return 1;}
void json_escape(char*out,size_t z,const char*in){size_t n=0;while(*in&&n+2<z){unsigned char c=(unsigned char)*in++;if(c=='\"'||c=='\\'){out[n++]='\\';out[n++]=(char)c;}else if(c>=32)out[n++]=(char)c;}out[n]=0;}
