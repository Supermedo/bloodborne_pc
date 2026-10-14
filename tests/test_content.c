#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#define REMOVE_DIR(path) _rmdir(path)
#define SET_ENV(name,value) _putenv_s(name,value)
#else
#include <unistd.h>
#define MAKE_DIR(path) mkdir(path,0755)
#define REMOVE_DIR(path) rmdir(path)
#define SET_ENV(name,value) setenv(name,value,1)
#endif
/* The user folder add-ons are listed from (runtime_file.c in the game): a temporary one here. */
static char user_dir[512];
const char *runtime_file_user_dir(void) { return user_dir; }
static const char *const test_dirs[]={"addcont/CUSA00900/SPEXPANSIONDLC03","addcont/CUSA00900","addcont",
                                     "dlc/SPEXPANSIONDLC03","dlc/SPDLCMESSENGER00","dlc",""};
static void remove_test_dirs(void) {
    char path[600];
    snprintf(path,sizeof(path),"%s/dlc/readme.txt",user_dir); remove(path);
    for (unsigned i=0;i<sizeof(test_dirs)/sizeof(*test_dirs);++i) {
        snprintf(path,sizeof(path),"%s/%s",user_dir,test_dirs[i]); REMOVE_DIR(path);
    }
}
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
#define GET(t,n) ((t)runtime_content_resolve(n))
int main(int argc,char **argv) {
    const char *temp=getenv("TEMP");
    snprintf(user_dir,sizeof(user_dir),"%s/bbport-content-test",temp && *temp ? temp : "/tmp");
    remove_test_dirs();
    SET_ENV("BB_DLC_DIR","");
    Module load=GET(Module,"g8cM39EUZ6o#M#N");
    Module loaded=GET(Module,"fMP5NHUOaMk#M#N");
    Module unload=GET(Module,"eR2bZFAAU0Q#M#N");
    Init init=GET(Init,"R9lA82OraNs#c#d");
    Param param=GET(Param,"99b82IKXpH4#c#d");
    List list=GET(List,"xnd8BJzAxmk#c#d");
    assert(load && loaded && unload && init && param && list);
    assert(!runtime_content_resolve("g8cM39EUZ6o#I#J"));
    if (argc>1 && !strcmp(argv[1],"--missing")) { load(0xb4); return 99; }
    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        puts("PASS: independent module references"); return 0;
    }
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    /* Add-ons: <user>/addcont/<title>/<label> and BB_DLC_DIR/<label> (the launcher's DLC folder),
       each label once; files and over-long names are not add-ons. */
    char path[600];
    for (int i=(int)(sizeof(test_dirs)/sizeof(*test_dirs))-1;i>=0;--i) {
        snprintf(path,sizeof(path),"%s/%s",user_dir,test_dirs[i]); assert(!MAKE_DIR(path));
    }
    snprintf(path,sizeof(path),"%s/dlc/readme.txt",user_dir);
    FILE *file=fopen(path,"w"); assert(file); fclose(file);
    snprintf(path,sizeof(path),"%s/dlc",user_dir); SET_ENV("BB_DLC_DIR",path);
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==2);
    memset(entries,0xaa,sizeof(entries));
    assert(list(0,entries,1,&h.hits)==0 && h.hits==1);
    assert(!memcmp(entries,"SPEXPANSIONDLC03\0\0\0\0\4\0\0\0",24));
    for (unsigned i=24;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==2);
    assert(!memcmp(entries+24,"SPDLCMESSENGER00\0\0\0\0\4\0\0\0",24));
    SET_ENV("BB_DLC_DIR","");
    remove_test_dirs();
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    return 0;
}
