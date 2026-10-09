/* Text and voice language overrides resolve independently and fall back to game files. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void variable(const char *key,const char *value) {
#ifdef _WIN32
    assert(!_putenv_s(key,value));
#else
    assert(!setenv(key,value,1));
#endif
}
static void folder(const char *path) { assert(!mkdir(path,0755)); }
static void file(const char *path) { FILE *f=fopen(path,"wb"); assert(f && !fclose(f)); }
static void path(char *out,size_t size,const char *root,const char *suffix) {
    assert((size_t)snprintf(out,size,"%s/%s",root,suffix)<size);
}
int main(void) {
    char root[512],p[1024],resolved[1024],expected[1024];
#ifdef _WIN32
    char temp[MAX_PATH];
    assert(GetTempPathA(sizeof(temp),temp));
    assert(GetTempFileNameA(temp,"bbp",0,root));
    assert(DeleteFileA(root));
    folder(root);
#else
    snprintf(root,sizeof(root),"/tmp/bbport-language-XXXXXX");
    assert(mkdtemp(root));
#endif
    path(p,sizeof(p),root,"dvdroot_ps4"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg/porbr"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg/engus"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg/porpt"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/sound"); folder(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg/porbr/menu.msgbnd.dcx"); file(p);
    path(p,sizeof(p),root,"dvdroot_ps4/msg/engus/item.msgbnd.dcx"); file(p);
    path(p,sizeof(p),root,"dvdroot_ps4/sound/sprj_c1020_eng.fsb"); file(p);
    path(p,sizeof(p),root,"dvdroot_ps4/sound/sprj_c1020_ptb.fsb"); file(p);
    runtime_file_mount("/app0",root);
    variable("BB_LANGUAGE","17");
    variable("BB_VOICE_LANGUAGE","auto");
    assert(!translate("/app0/dvdroot_ps4/msg/engus/menu.msgbnd.dcx",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/msg/porbr/menu.msgbnd.dcx");
    assert(!strcmp(resolved,expected));
    assert(!translate("/app0/dvdroot_ps4/msg/porpt/menu.msgbnd.dcx",resolved,sizeof(resolved)));
    assert(!strcmp(resolved,expected));
    assert(!translate("/app0/dvdroot_ps4/msg/engus/item.msgbnd.dcx",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/msg/engus/item.msgbnd.dcx");
    assert(!strcmp(resolved,expected));
    variable("BB_LANGUAGE","1");
    assert(!translate("/app0/dvdroot_ps4/msg/engus/menu.msgbnd.dcx",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/msg/engus/menu.msgbnd.dcx");
    assert(!strcmp(resolved,expected));
    assert(!translate("/app0/dvdroot_ps4/sound/sprj_c1020_eng.fsb",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/sound/sprj_c1020_eng.fsb");
    assert(!strcmp(resolved,expected));
    variable("BB_VOICE_LANGUAGE","ptb");
    assert(!translate("/app0/dvdroot_ps4/sound/sprj_c1020_eng.fsb",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/sound/sprj_c1020_ptb.fsb");
    assert(!strcmp(resolved,expected));
    variable("BB_LANGUAGE","17");
    variable("BB_VOICE_LANGUAGE","eng");
    assert(!translate("/app0/dvdroot_ps4/sound/sprj_c1020_ptb.fsb",resolved,sizeof(resolved)));
    path(expected,sizeof(expected),root,"dvdroot_ps4/sound/sprj_c1020_eng.fsb");
    assert(!strcmp(resolved,expected));
    variable("BB_VOICE_LANGUAGE","ded");
    assert(!translate("/app0/dvdroot_ps4/sound/sprj_c1020_eng.fsb",resolved,sizeof(resolved)));
    assert(!strcmp(resolved,expected));
    const char *created[]={"dvdroot_ps4/msg/porbr/menu.msgbnd.dcx",
                          "dvdroot_ps4/msg/engus/item.msgbnd.dcx",
                          "dvdroot_ps4/sound/sprj_c1020_eng.fsb",
                          "dvdroot_ps4/sound/sprj_c1020_ptb.fsb"};
    for (size_t i=0;i<sizeof(created)/sizeof(created[0]);++i) {
        path(p,sizeof(p),root,created[i]); assert(!remove(p));
    }
    const char *directories[]={"dvdroot_ps4/msg/porbr","dvdroot_ps4/msg/engus",
                               "dvdroot_ps4/msg/porpt","dvdroot_ps4/msg",
                               "dvdroot_ps4/sound","dvdroot_ps4"};
    for (size_t i=0;i<sizeof(directories)/sizeof(directories[0]);++i) {
        path(p,sizeof(p),root,directories[i]); assert(!rmdir(p));
    }
    assert(!rmdir(root));
    puts("Text and voice language files: independent selection and fallback PASS");
    return 0;
}
