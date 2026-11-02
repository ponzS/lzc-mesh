// Omarchy stores a private, owner-only key. Existing invalid keys are never
// silently replaced: doing so would defeat the account's device binding.
#include "identity.h"
#include "app.h"
#include "../../transport/include.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static MeshIdentity *identity;
static char *public_key,*fingerprint;
static bool fail(const char *why) {app_set_status(S_TRUST,ST_ERROR,"%s；共享已停止，未重置设备凭证",why);return false;}
bool identity_init(void) {
    if(identity)return true;
    char *dir=g_build_filename(g_get_user_config_dir(),"lzc-mesh",NULL);
    if(g_mkdir_with_parents(dir,0700)<0){g_free(dir);return fail("无法创建凭证目录");}
    char *path=g_build_filename(dir,"device-key",NULL);g_free(dir);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        struct stat st;char bytes[1025];ssize_t n=-1;
        if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() && !(st.st_mode&077) && st.st_nlink==1 && st.st_size>0 && st.st_size<=1024)
            n=read(fd,bytes,sizeof(bytes)-1);
        close(fd);g_free(path);
        if(n<=0 || n!=st.st_size)return fail("凭证文件权限或内容无效");
        bytes[n]=0;
        if(memchr(bytes,0,(size_t)n)){explicit_bzero(bytes,sizeof bytes);return fail("凭证文件无效");}
        identity=mesh_identity_new(bytes);explicit_bzero(bytes,sizeof bytes);
        if(!identity)return fail("无法读取原有设备凭证");
    } else {
        if(errno!=ENOENT){g_free(path);return fail("无法安全打开设备凭证");}
        MeshIdentity *created=mesh_identity_new(NULL);
        if(!created){g_free(path);return fail("无法生成设备凭证");}
        char *key=mesh_identity_export(created);size_t size=key?strlen(key):0;
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        bool ok=false;
        if(fd>=0) {
            ok=size>0 && write(fd,key,size)==(ssize_t)size && fsync(fd)==0;
            if(close(fd)<0)ok=false;
            if(!ok)unlink(path);
        }
        if(key){explicit_bzero(key,size);mesh_identity_string_free(key);}g_free(path);
        if(!ok){mesh_identity_free(created);return fail("无法安全保存设备凭证");}
        identity=created;
    }
    public_key=mesh_identity_public(identity);fingerprint=mesh_identity_fingerprint(identity);
    app_set_status(S_TRUST,ST_UNKNOWN,"等待批准 · 指纹 %s",fingerprint);
    return true;
}
const char *identity_public(void){return public_key?public_key:"";}
const char *identity_fingerprint(void){return fingerprint?fingerprint:"";}
char *identity_proof(const char *user,const char *nonce) {
    if(!identity)return NULL;
    char *value=mesh_identity_proof(identity,user,app.device_id,nonce);
    char *result=g_strdup(value);mesh_identity_string_free(value);return result;
}
void identity_shutdown(void) {
    mesh_identity_free(identity);identity=NULL;
    mesh_identity_string_free(public_key);public_key=NULL;
    mesh_identity_string_free(fingerprint);fingerprint=NULL;
}
