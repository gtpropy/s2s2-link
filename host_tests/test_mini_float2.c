#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
int mini_snprintf(char *, size_t, const char *, ...);
int main(void){
  srand(5); long bad=0,n=0;
  for(int it=0;it<300000;it++){
    double v; int k=rand()%4;
    if(k==0) v=(rand()/(double)RAND_MAX-0.5)*2000000.0;
    else if(k==1) v=(rand()%100000)/1000.0;            /* typical telemetry like 3.861 */
    else if(k==2) v=(rand()%2000)/8.0;                 /* many exact binary ties */
    else v=(rand()/(double)RAND_MAX)*1e-3;
    char f[16]; int p=rand()%7; snprintf(f,sizeof f,"%%.%df",p);
    char a[64],b[64]; snprintf(a,64,f,v); mini_snprintf(b,64,f,v); n++;
    if(strcmp(a,b)){ if(bad++<6) printf("diff %s v=%.17g libc=[%s] mini=[%s]\n",f,v,a,b);} }
  printf("random float cases: %ld, mismatches: %ld\n",n,bad); return bad!=0; }
