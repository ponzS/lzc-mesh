// Links the production Omarchy key-store adapter, with UI reporting only stubbed.
#include "../lzc-mesh-client/linux/src/app.h"
#include "../lzc-mesh-client/linux/src/identity.h"
#include <stdio.h>
#include <stdarg.h>
App app={.device_id="fixture-linux"};
void app_set_status(int which,StatusLevel level,const char *format,...) {
    if(level!=ST_ERROR)return;
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);
}
int main(int argc,char **argv) {
    if(!identity_init())return 1;
    printf("%s\n%s\n",identity_fingerprint(),identity_public());
    if(argc>1){char *proof=identity_proof("fixture-account",argv[1]);if(!proof)return 2;puts(proof);g_free(proof);}
    identity_shutdown();return 0;
}
