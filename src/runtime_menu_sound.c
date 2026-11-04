/* Local FSB5 ATRAC9 reader for two short, named Bloodborne cursor sounds.
 * Layout follows vgmstream's FSB5 parser; unsupported layouts fail closed. */
#include "runtime_menu_sound.h"
#include "../third_party/LibAtrac9/C/src/libatrac9.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t offset, bytes, frames, config; int channels, rate; } SoundEntry;
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const unsigned char *p){return (uint64_t)u32(p)|((uint64_t)u32(p+4)<<32);}
static int within(size_t at,size_t n,size_t len){return at<=len && n<=len-at;}

static int parse(const unsigned char *b,size_t len,SoundEntry *move,SoundEntry *select){
    if(!within(0,60,len)||memcmp(b,"FSB5",4)||u32(b+4)!=1||u32(b+24)!=13)return 0;
    const uint32_t count=u32(b+8),headers=u32(b+12),names=u32(b+16),data=u32(b+20);
    const size_t name_at=60u+(size_t)headers,data_at=name_at+(size_t)names;
    if(!count||count>4096||!within(60,headers,len)||!within(name_at,names,len)||
       !within(data_at,data,len)||data_at+(size_t)data!=len||names<count*4u)return 0;
    size_t pos=60;
    uint32_t found=0;
    for(uint32_t i=0;i<count;++i){
        if(!within(pos,8,name_at))return 0;
        const uint64_t mode=u64(b+pos);
        const uint32_t offset=(uint32_t)(((mode>>7)&0x07ffffffu)<<5);
        const uint32_t frames=(uint32_t)((mode>>34)&0x3fffffffu);
        const int channels=(int)((mode>>5)&3u)+1;
        const int rate_code=(int)((mode>>1)&15u);
        size_t next=pos+8;
        uint32_t config=0;int have_config=0;
        if(mode&1u){
            int more;
            do{
                if(!within(next,4,name_at))return 0;
                const uint32_t flag=u32(b+next),kind=flag>>25,size=(flag>>1)&0xffffffu;
                more=(int)(flag&1u);
                if(!within(next+4,size,name_at))return 0;
                if(kind==9 && size==8 && b[next+8]==0xfe){
                    config=(uint32_t)b[next+8]<<24|(uint32_t)b[next+9]<<16|
                           (uint32_t)b[next+10]<<8|b[next+11];
                    have_config=1;
                }
                next+=4u+size;
            }while(more);
        }
        const uint32_t rel=u32(b+name_at+4u*i);
        if(rel>=names)return 0;
        const char *name=(const char*)(b+name_at+rel);
        if(!memchr(name,0,names-rel))return 0;
        SoundEntry *target=NULL;
        if(!strcmp(name,"CURSOL_MOVE"))target=move;
        else if(!strcmp(name,"CURSOL_SELECT"))target=select;
        if(target){
            if(!have_config||channels!=1||rate_code!=9||!frames||frames>8192||offset>=data)return 0;
            target->offset=(uint32_t)(data_at+(size_t)offset);
            target->frames=frames;target->config=config;target->channels=channels;target->rate=48000;
            const uint32_t next_offset=i+1<count ?
                (within(next,8,name_at)?(uint32_t)(((u64(b+next)>>7)&0x07ffffffu)<<5):0) : data;
            if(next_offset<=offset||next_offset>data)return 0;
            target->bytes=next_offset-offset;
            found|=target==move?1u:2u;
        }
        pos=next;
    }
    return pos==name_at && found==3u;
}

static int decode(const unsigned char *b,size_t len,const SoundEntry *e,BbMenuSound *out){
    if(!within(e->offset,e->bytes,len))return 0;
    unsigned char config[4]={(unsigned char)(e->config>>24),(unsigned char)(e->config>>16),
                             (unsigned char)(e->config>>8),(unsigned char)e->config};
    void *decoder=Atrac9GetHandle();
    if(!decoder)return 0;
    Atrac9CodecInfo info={0};
    int ok=!Atrac9InitDecoder(decoder,config)&&!Atrac9GetCodecInfo(decoder,&info)&&
           info.channels==1&&info.samplingRate==48000&&info.frameSamples>0&&
           info.frameSamples<=256&&info.superframeSize>0&&info.superframeSize<=8192;
    int16_t *pcm=ok?calloc(e->frames,sizeof(*pcm)):NULL;
    if(!pcm)ok=0;
    uint32_t got=0,used_total=0;
    while(ok&&got<e->frames){
        unsigned char input[8192]={0};short frame[256]={0};int used=0;
        const uint32_t remain=e->bytes-used_total;
        if(!remain||remain>(uint32_t)sizeof(input)){ok=0;break;}
        memcpy(input,b+e->offset+used_total,remain);
        if(Atrac9Decode(decoder,input,frame,&used)||used<=0||(uint32_t)used>remain||
           got+(uint32_t)info.frameSamples>e->frames){ok=0;break;}
        memcpy(pcm+got,frame,(size_t)info.frameSamples*sizeof(*pcm));
        got+=(uint32_t)info.frameSamples;used_total+=(uint32_t)used;
    }
    Atrac9ReleaseHandle(decoder);
    if(!ok||got!=e->frames){free(pcm);return 0;}
    out->pcm=pcm;out->frames=got;return 1;
}

void runtime_menu_sound_free(BbMenuSounds *sounds){
    if(!sounds)return;
    free(sounds->move.pcm);free(sounds->select.pcm);
    memset(sounds,0,sizeof(*sounds));
}
int runtime_menu_sound_load(const char *bank_path,BbMenuSounds *out){
    if(!bank_path||!out)return 0;
    memset(out,0,sizeof(*out));
    FILE *f=fopen(bank_path,"rb");if(!f)return 0;
    if(fseek(f,0,SEEK_END)){fclose(f);return 0;}
    const long size=ftell(f);
    if(size<60||size>16*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return 0;}
    unsigned char *bank=malloc((size_t)size);
    if(!bank){fclose(f);return 0;}
    const int read_ok=fread(bank,1,(size_t)size,f)==(size_t)size;
    fclose(f);
    SoundEntry move={0},select={0};
    const int ok=read_ok&&parse(bank,(size_t)size,&move,&select)&&
                 decode(bank,(size_t)size,&move,&out->move)&&
                 decode(bank,(size_t)size,&select,&out->select);
    free(bank);
    if(!ok)runtime_menu_sound_free(out);
    return ok;
}
