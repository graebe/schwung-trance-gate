#include <stdio.h>
#include <string.h>
#include "audio_fx_api_v2.h"
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);
int main(void){ static host_api_v1_t h; memset(&h,0,sizeof h);
  audio_fx_api_v2_t*a=move_audio_fx_init_v2(&h); void*i=a->create_instance(NULL,NULL);
  static char b[65536]; if(a->get_param(i,"chain_params",b,sizeof b)>=0) printf("%s\n",b); return 0; }
