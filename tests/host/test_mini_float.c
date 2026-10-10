#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
int mini_snprintf(char *, size_t, const char *, ...);
int main(void){
  double v[]={0,0.5,1.0,3.86,3.8649,21.0,21.04,21.05,12.345,0.07,0.004,0.0049,99.995,5.0e-1,1234.5678,-3.86,-0.5,0.999,2.675,100.0, 1e9+0.5, 4294967.25};
  const char *f[]={"%.2f","%.1f","%.3f","%.0f","%6.2f","%f"};
  int bad=0,n=0;
  for(size_t i=0;i<sizeof v/sizeof*v;i++)for(size_t j=0;j<sizeof f/sizeof*f;j++){
    char a[64],b[64]; snprintf(a,64,f[j],v[i]); mini_snprintf(b,64,f[j],v[i]); n++;
    if(strcmp(a,b)){ if(bad++<12) printf("float diff fmt=%s v=%.10g libc=[%s] mini=[%s]\n",f[j],v[i],a,b);} }
  printf("float: %d cases, mismatches %d\n",n,bad); return 0; }
