/* Windows directory-open errors must depend on stat, not a previous call's errno.
 * After configuring the GPU build: ninja -C out/gpu file-windows-test
 * Then run: out/gpu/file-windows-test.exe
 * Or build independently of the GPU dependencies:
 * Run from the repository root in an MSYS2 CLANG64 shell:
 * clang -std=c11 -O2 -Wall -Wextra -Werror -UNDEBUG -pthread -Isrc \
 *   tests/test_file_windows.c src/compat_win.c -lbcrypt -o out/file-windows-test.exe
 * out/file-windows-test.exe
 */
#include "../src/runtime_file.c"
#include <assert.h>

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

int main(void) {
    char temp[MAX_PATH],root[MAX_PATH],file[MAX_PATH+32];
    DWORD length=GetTempPathA(sizeof(temp),temp);
    assert(length>0 && length<sizeof(temp));
    assert(GetTempFileNameA(temp,"bbp",0,root));
    assert(DeleteFileA(root) && CreateDirectoryA(root,NULL));
    snprintf(file,sizeof(file),"%s/regular.bin",root);
    FILE *f=fopen(file,"wb"); assert(f);
    assert(fputs("data",f)>=0 && !fclose(f));
    assert(!runtime_file_mount("/data",root));

    /* A successful stat need not clear errno. Each prior value must produce ENOTDIR. */
    const int prior_errors[]={0,ENOENT,EACCES};
    HostStat s;
    assert(!host_stat(file,&s) && S_ISREG(s.st_mode));
    for (size_t i=0;i<sizeof(prior_errors)/sizeof(*prior_errors);++i) {
        errno=prior_errors[i];
        assert(runtime_file_open("/data/regular.bin",0x20000,0)==-ENOTDIR);
    }

    /* A real stat failure must supply its own error, regardless of the prior errno. */
    snprintf(file,sizeof(file),"%s/missing.bin",root);
    assert(host_stat(file,&s)==-1 && errno==ENOENT);
    errno=EACCES;
    assert(runtime_file_open("/data/missing.bin",0x20000,0)==-ENOENT);

    errno=ENOENT;
    int dir=(int)runtime_file_open("/data",0x20000,0); assert(dir>=3);
    assert(!runtime_file_close(dir));
    int regular=(int)runtime_file_open("/data/regular.bin",0,0); assert(regular>=3);
    assert(!runtime_file_close(regular));

    runtime_file_unmount("/data");
    snprintf(file,sizeof(file),"%s/regular.bin",root);
    assert(DeleteFileA(file) && RemoveDirectoryA(root));
    puts("Windows guest files: directory opens ignore stale errno and preserve stat errors PASS");
}
