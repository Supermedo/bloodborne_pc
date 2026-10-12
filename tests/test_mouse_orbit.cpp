// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the orbit-position blend, including its generated ownership gate.
// Unlike angle-only fixtures, these checks observe the resulting world vector.
#include <windows.h>
#include <xbyak/xbyak.h>
#include <Zydis/Zydis.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "bbport_mouse.h"
#include "bbport_settings.h"
#include "mouse_camera_fixture.h"
#include "test_assert.h"

struct alignas(32) OrbitCase {
    std::array<unsigned char,0x300> camera{};
    alignas(16) std::array<float,4> old{}, target{}, rates{}, result{};
    alignas(32) std::array<std::uint64_t,4> preserved{};
    std::uint64_t flags{}, rax{}, r10{};
    std::uint32_t mxcsr{};
};
using RunOrbit = void (*)(OrbitCase*);

struct OrbitCaller : Xbyak::CodeGenerator {
    explicit OrbitCaller(void* block) {
        using namespace Xbyak::util;
        push(rbp); push(r12); push(r13); sub(rsp,0x2c0);
        vmovdqu(ptr[rsp + 0x260],ymm13);
        stmxcsr(ptr[rsp + 0x280]);
        mov(dword[rsp + 0x284],0x1f80); ldmxcsr(ptr[rsp + 0x284]);
        mov(r12,rcx); mov(r13,rcx); lea(rbp,ptr[rsp + 0x250]);
        vmovups(xmm4,ptr[r12 + offsetof(OrbitCase,old)]);
        vmovaps(ptr[rbp - 0x250],xmm4);
        vmovups(xmm13,ptr[r12 + offsetof(OrbitCase,target)]);
        vmovups(xmm4,ptr[r12 + offsetof(OrbitCase,rates)]);
        // The preceding game instructions supply 1-k. The position's fourth
        // lane is unchanged because both old and target positions contain 1.
        vpcmpeqd(xmm0,xmm0,xmm0); vpsrld(xmm0,xmm0,25); vpslld(xmm0,xmm0,23);
        vsubps(xmm0,xmm0,xmm4);
        mov(r10,0x12349876abcdef55);
        mov(rax,reinterpret_cast<uintptr_t>(block));
        stc(); call(rax);
        mov(ptr[r12 + offsetof(OrbitCase,rax)],rax);
        mov(ptr[r12 + offsetof(OrbitCase,r10)],r10);
        pushfq(); pop(rax); mov(ptr[r12 + offsetof(OrbitCase,flags)],rax);
        vmovups(ptr[r12 + offsetof(OrbitCase,result)],xmm0);
        vmovdqu(ptr[r12 + offsetof(OrbitCase,preserved)],ymm13);
        stmxcsr(ptr[r12 + offsetof(OrbitCase,mxcsr)]);
        ldmxcsr(ptr[rsp + 0x280]);
        vmovdqu(ymm13,ptr[rsp + 0x260]);
        add(rsp,0x2c0); pop(r13); pop(r12); pop(rbp); ret();
        ready();
    }
};

template<class T> void Put(OrbitCase& data, unsigned at, T value) {
    std::memcpy(data.camera.data() + at,&value,sizeof(value));
}
float Get(const OrbitCase& data, unsigned at) {
    float value; std::memcpy(&value,data.camera.data() + at,4); return value;
}
std::array<float,4> Vector(float pitch,float yaw,float radius=3.7f) {
    return {-std::sin(yaw)*std::cos(pitch)*radius,std::sin(pitch)*radius,
            -std::cos(yaw)*std::cos(pitch)*radius,1.f};
}
void Check(const OrbitCase& data,void* block) {
    assert(data.flags & 1);
    assert(data.rax == reinterpret_cast<uintptr_t>(block));
    assert(data.r10 == 0x12349876abcdef55);
    // The game's floating-point math may set status bits, but must not change
    // rounding, exception masks or denormal controls.
    assert((data.mxcsr & ~0x3fu) == 0x1f80);
    assert(!std::memcmp(data.preserved.data(),data.target.data(),16));
    for (unsigned n=16;n<32;++n) assert(reinterpret_cast<const unsigned char*>(data.preserved.data())[n]==0);
}
void Equal(const std::array<float,4>& a,const std::array<float,4>& b) {
    for (unsigned n=0;n<4;++n) assert(std::abs(a[n]-b[n])<2e-6f);
}

void Captured(unsigned char* image,const char* path) {
    std::ifstream file(path); assert(file.good());
    std::string line; std::size_t bytes=0, next=CameraFixture::Begin;
    while (std::getline(file,line)) {
        if (line.size()<12 || line[8]!=':') continue;
        const auto at=std::stoull(line.substr(0,8),nullptr,16);
        if (at<CameraFixture::Begin || at>=CameraFixture::End) continue;
        assert(at==next);
        const auto end=line.find(' ',10);
        const auto code=line.substr(10,end-10);
        assert(code.size()%2==0 && code.size()<=30);
        for (unsigned n=0;n<code.size();n+=2) image[next++]=std::stoul(code.substr(n,2),nullptr,16);
        bytes+=code.size()/2;
    }
    assert(bytes==CameraFixture::End-CameraFixture::Begin);
}

void CheckPatchScope(const unsigned char* original,const unsigned char* patched) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    unsigned changed=0;
    for (std::size_t at=0;at<CameraFixture::End-CameraFixture::Begin;) {
        ZydisDecodedInstruction inst{};
        ZydisDecodedOperand args[ZYDIS_MAX_OPERAND_COUNT]{};
        assert(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,original+at,
            CameraFixture::End-CameraFixture::Begin-at,&inst,args)));
        if (std::memcmp(original+at,patched+at,inst.length)) {
            ++changed;
            if (CameraFixture::Begin+at==CameraFixture::Orbit) {
                assert(inst.mnemonic==ZYDIS_MNEMONIC_VBROADCASTSS);
            } else if (at>=6) {
                assert(args[0].type==ZYDIS_OPERAND_TYPE_MEMORY && args[0].mem.base==ZYDIS_REGISTER_R13);
                const auto field=args[0].mem.disp.value;
                assert(field==0x140 || field==0x144 || field==0x148 || field==0x14c ||
                       field==0x150 || field==0x26c || field==0x270 || field==0x294);
                assert(inst.mnemonic==ZYDIS_MNEMONIC_VMOVSS || inst.mnemonic==ZYDIS_MNEMONIC_VEXTRACTPS ||
                       inst.mnemonic==ZYDIS_MNEMONIC_MOV);
            }
        }
        at+=inst.length;
    }
    assert(changed>30);
    std::puts("PASS: all collision, radial-distance and native override instructions unchanged");
}

void Replay(const char* path,RunOrbit run,OrbitCase& data,void* block) {
    std::ifstream file(path,std::ios::binary); assert(file.good());
    std::array<unsigned char,4096> header{};
    assert(bool(file.read(reinterpret_cast<char*>(header.data()),header.size())));
    const auto word=[&](unsigned at) { std::uint32_t value; std::memcpy(&value,header.data()+at,4); return value; };
    const bool v1=!std::memcmp(header.data(),"BBMOUSE1",8) && word(8)==1 && word(16)==336 && word(20)==4096;
    const bool v2=!std::memcmp(header.data(),"BBMOUSE2",8) && word(8)==2 && word(16)==552 && word(20)==16384;
    assert((v1 || v2) && word(12)==4096 && word(32)==64 && word(36)==9);
    struct Record {
        std::uint64_t sequence,ticks,camera;
        std::int32_t dx,dy;
        float sensitivity;
        std::uint32_t flags;
        float before[64],after[9];
    } record{};
    static_assert(sizeof(Record)==336);
    auto& settings=BbSettings::Get();
    unsigned owned=0,idle=0,reversals=0;
    int last_x=0;
    BbMouse::SetActive(false); BbMouse::Stick(128,128);
    data.old=Vector(0,0);
    for (unsigned i=0;i<word(20);++i) {
        assert(bool(file.read(reinterpret_cast<char*>(&record),sizeof(record))));
        if (v2) assert(bool(file.seekg(word(16)-sizeof(record),std::ios::cur)));
        if (!record.sequence) break;
        assert(record.sequence==i+1);
        for (unsigned n=0;n<64;++n) {
            const auto field=word(64+n*4); assert(field<=data.camera.size()-4);
            Put(data,field,record.before[n]);
        }
        settings.mouse_sensitivity=record.sensitivity;
        settings.mouse_invert_y=bool(record.flags&8);
        if (!(record.flags&2)) BbMouse::SetActive(false);
        BbMouse::SetActive(bool(record.flags&1));
        BbMouse::Stick(record.flags&4?0:128,128);
        BbMouse::Motion(float(record.dx)/256.f,float(record.dy)/256.f);
        BbMouse::Apply(data.camera.data());
        // Reuse the real raw counts and angle state. Only the isolated orbit
        // operator is executed, with a fixed focus/radius and recorded rates;
        // this does not replay level geometry or the complete game update.
        data.target=Vector(Get(data,0x140),Get(data,0x144));
        data.rates={Get(data,0x1ac),Get(data,0x1b8),Get(data,0x1ac),0};
        run(&data); Check(data,block);
        if ((record.flags&7)==3 && Get(data,0x154)!=1.f) {
            ++owned;
            Equal(data.result,data.target);
            for (unsigned n=0;n<7;++n) {
                const auto field=word(320+n*4); assert(field<=data.camera.size()-4);
                assert(std::abs(Get(data,field)-record.after[n])<2e-6f);
            }
            idle+=!record.dx && !record.dy;
            const int direction=(record.dx>0)-(record.dx<0);
            if (direction) { reversals+=last_x && last_x!=direction; last_x=direction; }
        }
        data.old=data.result;
    }
    assert(owned>0 && idle>0 && reversals>0);
    std::printf("PASS: recorded input/angle replay: %u mouse-owned updates, %u idle, %u horizontal reversals; isolated orbit output reaches target\n",owned,idle,reversals);
    settings.mouse_sensitivity=100.f; settings.mouse_invert_y=false;
    BbMouse::Stick(128,128); BbMouse::SetActive(false);
}

int main(int argc,char** argv) {
    auto* image=static_cast<unsigned char*>(VirtualAlloc(nullptr,CameraFixture::ImageSize,
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)); assert(image);
    CameraFixture::Populate(image);
    if (argc>1) Captured(image,argv[1]);
    std::array<unsigned char,CameraFixture::End-CameraFixture::Begin> original{};
    std::memcpy(original.data(),image+CameraFixture::Begin,original.size());
    assert(BbMouse::Install(image,CameraFixture::ImageSize));
    CheckPatchScope(original.data(),image+CameraFixture::Begin);
    if (argc>1) std::puts("PASS: actual captured camera routine accepted; no full game execution/resources required");
    // End only this isolated math block. The rest of the captured function is
    // never executed and no synthetic guest character/physics pointers are used.
    image[CameraFixture::Orbit+sizeof(CameraFixture::OrbitCode)]=0xc3;
    DWORD old;
    assert(VirtualProtect(image,CameraFixture::ImageSize,PAGE_EXECUTE_READ,&old));
    FlushInstructionCache(GetCurrentProcess(),image,CameraFixture::ImageSize);
    void* block=image+CameraFixture::Orbit;
    OrbitCaller caller(block); const auto run=caller.getCode<RunOrbit>();
    OrbitCase data;
    Put(data,0x1f0,-.6981317f); Put(data,0x1ec,1.2217305f);
    Put(data,0x130,.0f);
    auto& settings=BbSettings::Get(); settings.mouse_camera=true;
    data.old=Vector(0,0); data.target=Vector(.4f,.1f);
    data.rates={0.f,.03f,0.f,0.f};
    BbMouse::SetActive(true);
    // Reproduce the old game's asymmetric response before mouse ownership:
    // horizontal motion is held inside the band while vertical motion chases.
    run(&data); Check(data,block);
    assert(data.result[0]==data.old[0] && data.result[2]==data.old[2]);
    assert(data.result[1]>data.old[1] && data.result[1]<data.target[1]);
    const auto native=data.result;
    BbMouse::Motion(1,1); BbMouse::Apply(data.camera.data());
    run(&data); Check(data,block); Equal(data.result,data.target);
    assert(Get(data,0x130)==0.f); // do not overwrite native chase/timer parameters
    // Every raw count must affect the output vector immediately on either axis,
    // including direction reversal, wraparound and long stops. Rates deliberately
    // include the recorded dead-band and unequal chase cases.
    for (int fps:{30,60,120,240}) {
        for (auto rates:{std::array<float,4>{0,.03f,0,0},{.1f,.03f,.1f,0},{.25f,.03f,.25f,0}}) {
            data.rates=rates;
            Put(data,0x140,0.f); Put(data,0x144,0.f); data.old=Vector(0,0);
            for (int frame=0;frame<fps*2;++frame) {
                const float dx=frame<fps?1.f:-1.f,dy=frame<fps?1.f:-1.f;
                BbMouse::Motion(dx,dy); BbMouse::Apply(data.camera.data());
                data.target=Vector(Get(data,0x140),Get(data,0x144));
                run(&data); Check(data,block); Equal(data.result,data.target);
                data.old=data.result;
            }
            for (int frame=0;frame<fps;++frame) { run(&data); Equal(data.result,data.target); data.old=data.result; }
        }
    }
    // Handoff uses the original orbit response, also for vertical-only stick.
    data.old=Vector(0,0); data.target=Vector(.4f,.1f); data.rates={0,.03f,0,0};
    BbMouse::Stick(128,0); run(&data); Equal(data.result,native); Check(data,block);
    BbMouse::Stick(128,128); run(&data); Equal(data.result,native);
    BbMouse::Motion(1,1); BbMouse::Apply(data.camera.data());
    run(&data); Equal(data.result,data.target);
    Put(data,0x154,1.f); run(&data); Equal(data.result,native); Check(data,block);
    Put(data,0x154,0.f); BbMouse::SetActive(false); run(&data); Equal(data.result,native);
    BbMouse::SetActive(true); BbMouse::Motion(1,1); BbMouse::Apply(data.camera.data());
    // Pitch at wrap/limits and different camera radii still uses the same angles.
    for (float pitch:{-.6981317f,0.f,1.2217305f}) for (float yaw:{-3.1415f,0.f,3.1415f})
        for (float radius:{.2f,3.7f,8.f}) {
            data.old=Vector(0,0); data.target=Vector(pitch,yaw,radius);
            run(&data); Equal(data.result,data.target); Check(data,block);
        }
    if (argc>2) Replay(argv[2],run,data,block);
    Put(data,0x154,0.f); BbMouse::SetActive(true); BbMouse::Motion(1,1); BbMouse::Apply(data.camera.data());
    const auto start=std::chrono::steady_clock::now();
    for (unsigned n=0;n<100000;++n) run(&data);
    const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000;
    std::printf("Orbit math + ownership gate + ABI test wrapper: %.0f ns/update\n",ns);
    BbMouse::SetActive(false); VirtualFree(image,0,MEM_RELEASE);
    std::puts("PASS: real orbit-position output, one-count reversal, equal-axis gain, no idle chase, native stick/lock-on/toggle handoff, flags/registers/AVX and radius/angle limits");
}
