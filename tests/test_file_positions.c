/* One file read from two threads, with pread and with seek + read: every read gets the bytes of
 * the place it asked for (on Windows a pread used to move the place a seek of the other thread
 * had set). Then the position after read, write, append and seeks. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_quit_on_open(const char *guest) { (void)guest; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

#define WORDS (4u<<20) /* 16 MiB: word i holds i */
#define CHUNK 4096u
#define ROUNDS 20000
static int fd;
static volatile int done;

static uint32_t pick(uint32_t *state) {
    *state=*state*1664525u+1013904223u;
    return (*state>>8)%(WORDS-CHUNK/4);
}
static void *positioned_reads(void *unused) {
    (void)unused;
    uint32_t state=7,buffer[CHUNK/4];
    while (!done) {
        uint32_t word=pick(&state);
        assert(do_pread(fd,buffer,CHUNK,(int64_t)word*4)==CHUNK && buffer[0]==word);
    }
    return NULL;
}

int main(void) {
    const char *root="out/file-positions-test";
    char game[512],user[512],path[600];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(path,sizeof(path),"%s/words.bin",game);
    mkdir("out",0755); mkdir(root,0755); mkdir(game,0755); mkdir(user,0755);
    FILE *f=fopen(path,"wb"); assert(f);
    for (uint32_t i=0;i<WORDS;++i) assert(fwrite(&i,4,1,f)==1);
    assert(!fclose(f));
    runtime_file_configure(game,user);

    fd=(int)do_open("/app0/words.bin",0,0); assert(fd>=3);
    pthread_t other;
    assert(!pthread_create(&other,NULL,positioned_reads,NULL));
    uint32_t state=3,buffer[CHUNK/4];
    int wrong=0;
    for (int round=0;round<ROUNDS;++round) {
        uint32_t word=pick(&state);
        assert(do_lseek(fd,(int64_t)word*4,0)==(int64_t)word*4);
        if (do_read(fd,buffer,CHUNK)!=CHUNK || buffer[0]!=word) ++wrong;
    }
    done=1;
    assert(!pthread_join(other,NULL));
    if (wrong) { printf("FAIL: %d of %d seek + read next to pread read elsewhere\n",wrong,ROUNDS); return 1; }

    /* The position follows reads and seeks; a pread leaves it alone. */
    uint32_t word;
    assert(do_lseek(fd,8,0)==8 && do_read(fd,&word,4)==4 && word==2);
    assert(do_pread(fd,&word,4,400)==4 && word==100);
    assert(do_read(fd,&word,4)==4 && word==3);
    assert(do_lseek(fd,4,1)==20 && do_read(fd,&word,4)==4 && word==5);
    assert(do_lseek(fd,-4,2)==(int64_t)WORDS*4-4 && do_read(fd,&word,4)==4 && word==WORDS-1);
    assert(do_read(fd,&word,4)==0);
    assert(do_lseek(fd,-1,0)==-EINVAL && do_lseek(fd,0,1)==(int64_t)WORDS*4);
    assert(!do_close(fd));

    /* Writes: at the position, pwrite at its place, append at the end. */
    int w=(int)do_open("/data/positions",0x602,0644); assert(w>=3);
    char text[16]={0};
    assert(do_write(w,"abc",3)==3 && do_lseek(w,0,1)==3);
    assert(do_pwrite(w,"X",1,0)==1 && do_lseek(w,0,1)==3);
    assert(do_write(w,"d",1)==1);
    assert(do_lseek(w,0,0)==0 && do_read(w,text,8)==4 && !strcmp(text,"Xbcd"));
    assert(!do_close(w));
    w=(int)do_open("/data/positions",0x9,0); assert(w>=3);
    assert(do_write(w,"ef",2)==2 && !do_close(w));
    w=(int)do_open("/data/positions",0,0); assert(w>=3);
    memset(text,0,sizeof(text));
    assert(do_read(w,text,8)==6 && !strcmp(text,"Xbcdef") && !do_close(w));

    char data[600]; snprintf(data,sizeof(data),"%s/data/positions",user);
    assert(!remove(data) && !remove(path));
    printf("File positions: %d seek + read next to pread at their place; read, write, append and seeks PASS\n",ROUNDS);
    return 0;
}
