// SPDX-License-Identifier: GPL-2.0-or-later
// LocalChalice offline glyph search (game 1.09; port-independent game layer + lc_platform).
//
// Offline, the altar's "Search by Chalice Glyph" (text 262002) is greyed and its handler shows the
// game's "cannot be used in offline mode" box. Here, only while offline, everything happens in the
// game's own flow and UI:
//   1. the item is enabled (builder 0x1b66470: the offline branch stores "greyed = 0");
//   2. its handler (0x1b687f0) goes on, as online, to the game's own glyph box and its
//      "Search using Chalice Glyph: <glyph>" OK / CANCEL confirmation;
//   3. after OK the flow's continuation (0x1b70da0, which would start the network search) is
//      replaced: the glyph is read from the flow's own state, the dungeon is written to the current
//      altar slot's record (flag 9020, as the game's Join 0x1c0fb90 picks it), that altar's ritual
//      helper reloads it in place with the game's own Resume request (0x1aa40e0, the load path),
//      and the result is returned as the game's own OK message box (0x1c1f500).
// Online play is untouched: the patches only change offline branches and the continuation runs
// the original whenever the FROM client has a session. Nothing is armed for BB_ONLINE=1 or BBSC.
#include "lc_desc.h"
#include "lc_platform.h"
#include "offline_slot.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using U = uintptr_t;
#define SYSV __attribute__((sysv_abi))
using Step = U (SYSV *)(U);
using Call6 = U (SYSV *)(U,U,U,U,U,U);
using Get = uint32_t (SYSV *)(U, uint32_t, uint32_t);
using Set = void (SYSV *)(U, uint32_t, uint32_t, uint32_t);
using Resume = bool (SYSV *)(U);
using Update = void (SYSV *)(U, void*);
using Text = void (SYSV *)(void*, int, int);
using MessageBox = U (SYSV *)(U, U, void*);
using Free = void (SYSV *)(U, U);
using Then = void (SYSV *)(U*, U*, U*);
using CodeStep = void (SYSV *)(U*, void*, int);

static unsigned char* base;

// Image offsets (1.09 eboot).
constexpr U kNetMan = 0x553b120, kFlagMan = 0x553b100, kHolygrail = 0x553e9e8;
constexpr U kGetFlag = 0x13cfd80, kSetFlag = 0x13d0060;
constexpr U kWaitForRequest = 0x1aa64c0;   // CSDungeonRitualHelper::STEP_Wait_forRequest
constexpr U kResume = 0x1aa40e0;           // request 2 (needs status 0, as during a load)
constexpr U kTimeVtable = 0x52efd30 + 0x10; // the dt object the game's requesters pass to Update
constexpr U kText = 0x1ae8cc0;             // text(out 0x40-byte text object, FMG, id)
constexpr U kOkBox = 0x1c1f500;            // OK message box action(out, menu context, text object)
constexpr U kContinuation = 0x1b70da0;     // glyph flow, after the confirmation: starts the search
// After a successful placement: "<OK message> then close the altar menu", so the next visit
// opens the occupied-altar page (the altar root picks its page once, 0x1b74e80 / 0x1b75280).
// Closing writes result code 3 to widget +0x1c0 (what the game's close-page callback 0x1b8a1c0
// does), which the page step (0x1c1d420) reads to finish the page.
//
// Checks. A function the mod only calls is recognised by 16 bytes of its body at +0x18, not by
// its entry: a fork may hook or replace a function's first bytes and keep it working (bbhost
// hooks the message lookup and replaces the event flag readers with its own code), and calling
// it then still does what the game's own code does. A function the mod hooks, and every byte it
// patches, is checked exactly: its first bytes are copied into the trampoline.
constexpr size_t kBodyAt = 0x18;
// The menu context handed to the flow is the page widget's secondary base at +0x48: the page
// lambda (0x1b75bd0) sets vptr 0x539b560 at +0 and 0x539b5e0 at +0x48 (test5 log: +539b5e0).
constexpr U kWidgetVptr = 0x539b560, kContextVptr = 0x539b5e0, kContextOffset = 0x48;
// Page step update (0x1c1d420): widget = step +0xd0; the page finishes when widget +0x1c0 >= 2.
// test6: writing 3 from inside the item's flow did not close the page, so the close is applied
// here, by the page's own update, once the OK box is gone (signalled by the close lambda, which
// is given our signal buffer instead of the widget: it writes 3 to signal +0x1c0).
// The glyph list (offline "Chalice Dungeon Search", text 262001): pages built by the game's own
// altar-page builder (0x1b66470), whose items are replaced during that call in its add-item
// function (0x1b76150). An item's handler is a std::function of the game's function-pointer type
// (__func vtable 0x533d120; its call 0x1b6ee10 runs fn(&result, page widget + 0x48)); each item gets
// a stub that loads its index into rdx and jumps to a shared handler.
constexpr U kAddItem = 0x1b76150, kSearchHandler = 0x1b68200, kAltarPage = 0x1b66470;
constexpr U kFnVtable = 0x533d120, kFnCall = 0x1b6ee10;
constexpr unsigned char add_head[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                      0x48,0x81,0xec,0x88,0x01,0x00,0x00};
constexpr unsigned char search_head[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                         0x48,0x81,0xec,0x48,0x01,0x00,0x00};
constexpr unsigned char altar_body[] = {0x89,0xbd,0xb0,0xeb,0xff,0xff,0x48,0x8b,0x05,0x4b,0xd7,0x87,0x03,0x48,0x8b,0x00};
// Builder, item 262001: cmpb $0,8(%rax); mov $1,%r14b; je +0x1d (greyed) ... setne %r14b at 0x1b6666f.
constexpr U kEnable2Patch = 0x1b6664d, kEnable2Byte = 0x1b66655;
constexpr unsigned char enable2_before[] = {0x80,0x78,0x08,0x00,0x41,0xb6,0x01,0x74,0x1d,0x80,0xb8,0xf8,0x09,0,0,0,0x75,0x14};
constexpr unsigned char enable2_target[] = {0x41,0x0f,0x95,0xc6}; // at 0x1b6666f
constexpr U kPageUpdate = 0x1c1d420;
constexpr unsigned char page_head[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                       0x48,0x81,0xec,0xd8,0x00,0x00,0x00};
// The game's message lookup (kText calls it): lookup(repository, 0, fmg, id) -> the loaded UTF-16
// text, or null. Pure reads of the loaded FMG tables (repository at 0x5540300).
constexpr U kMsgLookup = 0xf6f340, kMsgRepository = 0x5540300;
constexpr unsigned char lookup_body[] = {0x8b,0x04,0xc6,0x31,0xc0,0x4d,0x85,0xc0,0x74,0x79,0x45,0x31,0xdb,0x41,0x39,0x48};
constexpr unsigned char lea_r15[] = {0x4c,0x8d,0x3d};
constexpr uint32_t kSlotFlag = 9020;       // 7 bits, one-hot: the current altar slot
constexpr int kDialogFmg = 204, kOfflineNotice = 262002;

constexpr unsigned char idle_head[] = {0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0xc7,0x83,0x64,0x02,0,0,0,0,0,0};
constexpr unsigned char cont_head[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x48,0x49,0x89,0xfe};
constexpr unsigned char get_body[] = {0xaf,0xc1,0x29,0xc6,0x42,0x8d,0x04,0x06,0x4c,0x8b,0x7f,0x38,0x49,0x8d,0x5f,0x08};
constexpr unsigned char set_body[] = {0xc1,0x41,0x0f,0xaf,0xca,0x29,0xce,0x42,0x8d,0x14,0x0e,0x48,0x8b,0x4f,0x38,0x48};
constexpr unsigned char resume_body[] = {0x48,0x89,0x45,0xe8,0x83,0xbb,0x64,0x02,0x00,0x00,0x00,0x74,0x04,0x31,0xc0,0xeb};
constexpr unsigned char text_body[] = {0xfe,0x4c,0x8b,0x2d,0x00,0xaf,0x8f,0x03,0x49,0x8b,0x45,0x00,0x48,0x89,0x45,0xd0};
constexpr unsigned char box_body[] = {0xc4,0x46,0x7c,0x03,0x49,0x8b,0x04,0x24,0x48,0x89,0x45,0xd8,0xc7,0x85,0xa0,0xed};
// Builder, item 262002: cmpb $0,8(%rax); mov $1,%cl; je +0x1c (-> greyed = 1) ... setne %cl.
constexpr U kEnablePatch = 0x1b6678b;
constexpr unsigned char enable_before[] = {0x80,0x78,0x08,0x00,0xb1,0x01,0x74,0x1c,0x80,0xb8,0xf8,0x09,0,0,0,0x75,0x13};
constexpr U kEnableByte = 0x1b66792;       // je target 0x1b667af -> 0x1b667ac (setne %cl with ZF=1: 0)
constexpr unsigned char enable_target[] = {0x0f,0x95,0xc1,0x88,0x8d,0xa8,0xeb,0xff,0xff}; // at 0x1b667ac
// Handler 0x1b687f0: cmpb $0,8(%rax); je notice. NOP'd: offline goes on (the other checks are clear).
constexpr U kHandlerPatch = 0x1b6884b;
constexpr unsigned char handler_before[] = {0x80,0x78,0x08,0x00,0x74,0x1b,0x80,0xb8,0xf8,0x09,0,0,0,0x75,0x12};

static Step original_idle;
static Call6 original_continuation;
static Call6 original_page_update;
static Call6 original_add_item;
static Call6 original_search;
// Menu items: label and help (views into glyph_labels / groups), handler stub.
struct Item { const char16_t* label; const char16_t* help; U stub; };
static std::vector<Item> group_items, glyph_items;
static const Item* building=nullptr;  // the page being built (game thread)
static size_t building_count=0;
static int building_calls=0;
static U altar_context=0, group_context=0; // the pages the list was opened from
// Close request (game thread only): 0 none, 1 waiting for the OK box, 2 closing the page.
static int close_state=0, close_lines=0, close_tries=0;
static U close_widget=0, close_box=0, close_step=0, close_last=~U(0);
static bool close_held=false;
// Further pages to close after close_widget (the glyph list's parents), in order.
static U close_queue[4]; static int close_queued=0;

// Catalog v4 (make_catalog_v4.py): sorted records, description recipes, language table.
struct Entry { char glyph[8]; uint64_t channel; uint32_t form[31]; uint32_t name[4]; uint32_t item_first, item_count; };
static std::vector<Entry> catalog;
static std::vector<lc_desc::Item> desc_items;
static std::vector<std::pair<uint32_t,uint32_t>> desc_probes;
static std::vector<lc_desc::Language> desc_languages;
static const lc_desc::Language* desc_language=nullptr;
static std::vector<bool> desc_done;
// Menu texts. Labels are built at load; a group's descriptions are composed from the game's text
// (in its language) when the group is first opened, before its page exists. The game's text
// objects point into these strings, so a string never changes once its page has been built.
static std::vector<std::u16string> glyph_labels, glyph_helps;
struct Group { char first; size_t begin, count; std::u16string label, help; };
static std::vector<Group> groups;

static std::atomic<U> watch_helper{0};
static std::atomic<bool> idle_seen{false};

template<typename T> bool peek(U p, T& value) { return lc_platform::read(&value, p, sizeof value); }
U image() { return reinterpret_cast<U>(base); }
// The host's log and code writer when it provides them (a bbhost plugin: lc_set_host), else
// stdout (the port's log) and lc_platform::write_code.
using Logger = void (*)(const char*);
using CodeWriter = bool (*)(void* dst,const void* src,size_t n);
static Logger host_log=nullptr;
static CodeWriter host_write=nullptr;
void log_line(const char* text) {
    if(host_log) { host_log(text); return; }
    std::printf("LocalChalice offline: %s\n",text);
    std::fflush(stdout);
}
bool write_code(void* dst,const void* src,size_t n) {
    return host_write ? host_write(dst,src,n) : lc_platform::write_code(dst,src,n);
}
// FrpgNetMan +8: the FROM client's session (what the game's own checks read). Clear = offline.
bool offline() {
    U net=0; uint8_t session=1;
    return peek(image()+kNetMan,net) && net && peek(net+8,session) && !session;
}

// Flags are stored in 1000-bit blocks; a multi-bit range across a block boundary (record word 31,
// bits 992..1023) lands shifted in the save through the game's functions. Split it, MSB first.
bool read_flag(void* ctx,uint32_t id,uint32_t bits,uint32_t* value) {
    const auto get=reinterpret_cast<Get>(base+kGetFlag);
    const U man=reinterpret_cast<U>(ctx);
    const uint32_t head=1000-id%1000;
    if(bits<=head) { *value=get(man,id,bits); return true; }
    const uint32_t tail=bits-head;
    *value=(get(man,id,head)<<tail)|get(man,id+head,tail);
    return true;
}
bool write_flag(void* ctx,uint32_t id,uint32_t bits,uint32_t value) {
    const auto set=reinterpret_cast<Set>(base+kSetFlag);
    const U man=reinterpret_cast<U>(ctx);
    const uint32_t head=1000-id%1000;
    if(bits<=head) { set(man,id,bits,value); return true; }
    const uint32_t tail=bits-head;
    set(man,id,head,value>>tail);
    set(man,id+head,tail,value&((1u<<tail)-1));
    return true; // The core verifies each write through read_flag.
}

const Entry* find(const std::string& glyph) {
    auto it=std::lower_bound(catalog.begin(),catalog.end(),glyph,[](const Entry& e,const std::string& g){
        return std::memcmp(e.glyph,g.data(),8)<0; });
    return it!=catalog.end() && !std::memcmp(it->glyph,glyph.data(),8) ? &*it : nullptr;
}
// The record the game wrote when v0.5 joined a catalog dungeon (verified against the save for
// 83ss4pyq and 9kv8xiyi): map, channel, FormData words 2..25, creator "LocalHunter", the
// 0x30-byte tail block (header 0x0002/0x0030, 5, glyph text, 1, 1, 0x117ea, 0x117ea).
offline_slot::Record build(const Entry& e) {
    offline_slot::Record r{};
    r[0]=e.form[2];
    r[1]=static_cast<uint32_t>(e.channel); r[2]=static_cast<uint32_t>(e.channel>>32);
    for(int i=0;i<24;++i) r[3+i]=e.form[2+i];
    // Words 23..26 (creator's online ID) come from the FormData as they are. Words 27..30 hold the
    // creator's character name: the original where the catalog has it (Noxde), else "Unknown"
    // (empty made the game show the player's own name).
    if(e.name[0]||e.name[1]||e.name[2]||e.name[3]) for(int i=0;i<4;++i) r[27+i]=e.name[i];
    else { r[27]=0x6e6b6e55; r[28]=0x006e776f; } // "Unknown"
    r[31]=0x02003000; r[32]=5;
    std::memcpy(&r[33],e.glyph,8);
    r[39]=1; r[40]=1; r[41]=0x117ea; r[42]=0x117ea;
    return r;
}

// The mod's own sentences (messages.json order), and "not in the catalog" (the game's own
// "No dungeons found using the given Chalice Glyph.", menu FMG 204 id 4603).
enum MessageId { kGroupHelp, kNotReady, kNoAltar, kOccupied, kBusy, kWriteFailed, kNotPlaced, kPlaced, kSavedReload,
                 kInvalid, kMessages, kNotFound = 100 };
constexpr uint32_t kNotFoundFmg = 204, kNotFoundId = 4603;
struct Message { int id; std::string glyph; };

const char16_t* game_text(void*,uint32_t fmg,uint32_t id);
// The game's language (recognised once the game's text is loaded), else English (the first).
const lc_desc::Language& language() {
    if(desc_language) return *desc_language;
    U repository=0;
    if(desc_languages.empty()) { static const lc_desc::Language none{}; return none; }
    if(!peek(image()+kMsgRepository,repository) || !repository) return desc_languages.front();
    const uint32_t hash=lc_desc::probe_hash(desc_probes,game_text,nullptr);
    desc_language=&lc_desc::detect(desc_languages,hash);
    char line[96];
    std::snprintf(line,sizeof line,"texts in the game's language (%s%s)",desc_language->code.c_str(),
                  desc_language->probe==hash?"":", not recognised: English");
    log_line(line);
    return *desc_language;
}
std::u16string fill(std::u16string s,const std::string& glyph,char first) {
    for(size_t at; (at=s.find(u"{g}"))!=std::u16string::npos;) s.replace(at,3,std::u16string(glyph.begin(),glyph.end()));
    for(size_t at; (at=s.find(u"{c}"))!=std::u16string::npos;) s.replace(at,3,std::u16string(1,char16_t(first)));
    return s;
}
// The message in the given language (English for the log).
std::u16string say(const lc_desc::Language& lang,const Message& m) {
    if(m.id==kNotFound) {
        if(&lang!=&desc_languages.front()) if(const char16_t* t=game_text(nullptr,kNotFoundFmg,kNotFoundId)) if(*t) return t;
        return fill(u"Chalice Glyph {g} is not in the local catalog.",m.glyph,0);
    }
    if(m.id<0 || static_cast<size_t>(m.id)>=lang.messages.size()) return u"?";
    return fill(lang.messages[m.id],m.glyph,0);
}
std::string ascii(const std::u16string& s) {
    std::string out;
    for(char16_t c:s) out+=c=='\n'?' ':c<0x80?static_cast<char>(c):'?';
    return out;
}

U helper_of(int slot) {
    U grail=0,arr=0,obj=0; int s=-1;
    if(!peek(image()+kHolygrail,grail) || !grail || !peek(grail+8,arr) || !arr ||
       !peek(arr+8*static_cast<U>(slot),obj) || !obj || !peek(obj+0x108+0xc8,s) || s!=slot) return 0;
    return obj+0x108;
}

// On the game thread (the altar menu's flow). Returns the message for the game's message box;
// placed is set when the altar now holds the dungeon.
Message place(const std::string& glyph,bool& placed) {
    placed=false;
    char line[256];
    const Entry* e=find(glyph);
    if(!e) return {kNotFound,glyph};
    U man=0; int mode=-1;
    if(!peek(image()+kFlagMan,man) || !man || !peek(man+0x80,mode) || mode!=0) return {kNotReady,glyph};
    uint32_t bits=0;
    read_flag(reinterpret_cast<void*>(man),kSlotFlag,7,&bits);
    int width=0; for(uint32_t v=bits;v;v>>=1) ++width;
    const int slot=7-width; // as the game's Join (0x1c0fb90)
    const U helper=slot>=1 && slot<=6 ? helper_of(slot) : 0;
    uint32_t status=0,request=1,busy=1;
    if(!helper || !peek(helper+0x25c,status) || !peek(helper+0x260,request) || !peek(helper+0x264,busy)) {
        std::snprintf(line,sizeof line,"no altar for slot bits %02x (slot %d)",bits,slot); log_line(line);
        return {kNoAltar,glyph};
    }
    if(status!=1) return {kOccupied,glyph};
    if(request || busy) return {kBusy,glyph};

    const auto record=build(*e);
    const offline_slot::Flags flags{reinterpret_cast<void*>(man),read_flag,write_flag};
    const auto out=offline_slot::install(flags,slot,record);
    std::snprintf(line,sizeof line,"glyph %s -> slot %d: result %d",glyph.c_str(),slot,static_cast<int>(out.result));
    log_line(line);
    using R=offline_slot::Result;
    if(out.result==R::write_failed_unrestored) return {kWriteFailed,glyph};
    if(out.result!=R::installed) return {kNotPlaced,glyph};
    // Reload this altar from its record in place: status 0 + the game's Resume request, then its
    // Update until the helper is back in Wait_forRequest (what a character load does).
    uint32_t zero=0;
    std::memcpy(reinterpret_cast<void*>(helper+0x25c),&zero,4);
    struct { U vptr; float dt; } dt{image()+kTimeVtable,1.0f/30.0f};
    const auto update=*reinterpret_cast<Update*>(*reinterpret_cast<U*>(helper)+0xd0);
    idle_seen.store(false); watch_helper.store(helper);
    const bool resumed=reinterpret_cast<Resume>(base+kResume)(helper);
    idle_seen.store(false);
    int steps=0;
    for(; resumed && steps<600 && !idle_seen.load(); ++steps) update(helper,&dt);
    watch_helper.store(0);
    peek(helper+0x25c,status);
    std::snprintf(line,sizeof line,"slot %d refresh: resume %d, %d update(s), idle %d, status %u",slot,
                  static_cast<int>(resumed),steps,static_cast<int>(idle_seen.load()),status);
    log_line(line);
    if(status==5) {
        placed=true;
        return {kPlaced,glyph};
    }
    return {kSavedReload,glyph};
}

// The glyph the flow holds (shared state S: +0 menu context, +8 text object: +0 view pointer,
// +0x10 inline buffer or pointer, +0x28 capacity), read as 0x1b710e0 reads it.
bool flow_glyph(U state,std::string& glyph) {
    U view=0,cap=0,data=0;
    if(!peek(state+8,view)) return false;
    if(view) data=view;
    else if(!peek(state+0x30,cap)) return false;
    else if(cap>=8) { if(!peek(state+0x18,data)) return false; }
    else data=state+0x18;
    glyph.clear();
    for(int i=0;i<32;++i) {
        uint16_t c=0;
        if(!peek(data+2*static_cast<U>(i),c)) return false;
        if(!c) break;
        if(c<0x80 && !std::isspace(c)) glyph+=static_cast<char>(std::tolower(c));
        else if(c>=0x80) glyph+='?';
    }
    return true;
}

// The game's OK message box with our text: a real text object from the game's lookup, its view
// pointer (+0) set to ours, freed as the game frees it.
U message_box(U out,U context,const std::u16string& text) {
    static char16_t buffer[512];
    size_t n=0;
    for(char16_t c:text) if(n+1<sizeof buffer/sizeof *buffer) buffer[n++]=c;
    buffer[n]=0;
    alignas(16) unsigned char object[0x40]={};
    reinterpret_cast<Text>(base+kText)(object,kDialogFmg,kOfflineNotice);
    const U view=reinterpret_cast<U>(buffer);
    std::memcpy(object,&view,8);
    const U result=reinterpret_cast<MessageBox>(base+kOkBox)(out,context,object);
    U cap=0,alloc=0,data=0;
    std::memcpy(&cap,object+0x28,8); std::memcpy(&alloc,object+0x30,8); std::memcpy(&data,object+0x10,8);
    if(cap>=8 && alloc) reinterpret_cast<Free>(*reinterpret_cast<U*>(*reinterpret_cast<U*>(alloc)+0x70))(alloc,data);
    return result;
}

// Refcounted game action (count at +8, vtable[0] destroys), released as the game does.
void release(U action) {
    if(!action) return;
    if(__atomic_fetch_sub(reinterpret_cast<int32_t*>(action+8),1,__ATOMIC_ACQ_REL)==1)
        reinterpret_cast<Step>(*reinterpret_cast<U*>(*reinterpret_cast<U*>(action)))(action);
}
// The page widget of a menu context (the widget's interface at +0x48), or 0 when it is not one.
U widget_of(U context) {
    U vptr=0,widget_vptr=0;
    if(!context || !peek(context,vptr) || vptr!=image()+kContextVptr) return 0;
    const U widget=context-kContextOffset;
    return peek(widget,widget_vptr) && widget_vptr==image()+kWidgetVptr ? widget : 0;
}
// "<message box> then close the menu". Falls back to the message alone (the altar is already
// placed; the player leaves the menu) when the context is not a known menu widget. parents:
// further pages to close afterwards, innermost first (the glyph list's group page, the altar).
U message_then_close(U out,U context,const std::u16string& text,const U* parents=nullptr,int parent_count=0) {
    U vptr=0,widget_vptr=0;
    peek(context,vptr);
    const U widget=context-kContextOffset;
    peek(widget,widget_vptr);
    const bool known=vptr==image()+kContextVptr && widget_vptr==image()+kWidgetVptr;
    close_queued=0;
    for(int i=0;known && i<parent_count && close_queued<4;++i)
        if(const U w=widget_of(parents[i])) close_queue[close_queued++]=w;
    char line[160];
    std::snprintf(line,sizeof line,"menu context vtable +%llx, widget vtable +%llx: %s",
                  static_cast<unsigned long long>(vptr-image()),static_cast<unsigned long long>(widget_vptr-image()),
                  known?"closing the altar menu":"unknown, message only");
    log_line(line);
    message_box(out,context,text);
    if(!known) return out;
    // test7: a step chained after the OK box never runs (the box ends the flow), so the box is
    // returned alone, as the message-only path does, and watched: one extra reference is kept;
    // when the flow has released the box (count back to 1, ours) the page is finished from the
    // page step's update (page_update_hook).
    U box=0;
    std::memcpy(&box,reinterpret_cast<void*>(out),8);
    if(!box) return out;
    __atomic_fetch_add(reinterpret_cast<int32_t*>(box+8),1,__ATOMIC_ACQ_REL);
    if(close_box) release(close_box); // an earlier request that never completed
    close_box=box; close_widget=widget; close_step=0; close_held=false; close_state=1; close_lines=0; close_tries=0; close_last=~U(0);
    return out;
}

// The page step's update: after the OK box of a placement, finish the altar page with code 3
// (as the game's close-page callback does). States: 1 waiting for the flow to release the OK
// box, 2 write code 3 once, 3 cleared on the page's next update (never writes again).
U SYSV page_update_hook(U step,U dt,U c,U d,U e,U f) {
    // The widget is read before the update: a finishing page clears step +0xd0 in its cleanup
    // (test8: the finish went unseen, the request stayed armed and closed the next altar page
    // that reused the address).
    U widget=0;
    const bool ours=close_state && peek(step+0xd0,widget) && widget==close_widget &&
                    (!close_step || step==close_step);
    const U result=original_page_update(step,dt,c,d,e,f);
    if(!ours) return result;
    close_step=step;
    U value=0;
    int32_t refs=-1;
    U still=0;
    if(static_cast<uint32_t>(result)<2 && peek(step+0xd0,still) && still==widget) peek(widget+0x1c0,value);
    if(close_box) peek(close_box+8,refs); // we hold a reference: the object is alive
    char line[160];
    const U seen=(value<<8)^(result&0xff)^(static_cast<U>(refs&0xff)<<4)^static_cast<U>(close_state);
    if(seen!=close_last && close_lines<40) {
        std::snprintf(line,sizeof line,"page close: state %d, step result %llx, page result %llx, box refs %d",
                      close_state,static_cast<unsigned long long>(result),static_cast<unsigned long long>(value),refs);
        log_line(line); ++close_lines; close_last=seen;
    }
    if(static_cast<uint32_t>(result)>=2 || close_state==3) { // the page finished (or was told to)
        if(close_box) { release(close_box); close_box=0; }
        if(close_queued>0) { // then its parent page, when that one updates again
            close_widget=close_queue[0];
            for(int i=1;i<close_queued;++i) close_queue[i-1]=close_queue[i];
            --close_queued;
            close_step=0; close_state=2;
            std::snprintf(line,sizeof line,"page close: page finished (step result %llx); closing its parent page",
                          static_cast<unsigned long long>(result));
            log_line(line);
            return result;
        }
        std::snprintf(line,sizeof line,"page close: altar page finished (step result %llx); request cleared",
                      static_cast<unsigned long long>(result));
        log_line(line);
        close_state=0; close_widget=0; close_step=0;
        return result;
    }
    if(close_state==1) {
        if(refs>=2) close_held=true;     // this page's flow holds the OK box
        if(refs==1 && close_held) { // ...and has let go of it: it was closed
            release(close_box); close_box=0; close_state=2;
            log_line("page close: OK box closed, closing the altar page");
        } else if(++close_tries>36000) { // ~10 min at 60 FPS
            log_line("page close: OK box still held; giving up");
            release(close_box); close_box=0; close_state=0; close_widget=0; close_queued=0;
            return result;
        }
    }
    if(close_state==2) {
        // Once: the page reads it in its next update and finishes. Then the request is cleared
        // on that update whatever happens, so it can never touch another page.
        const U three=3;
        if(still==widget) std::memcpy(reinterpret_cast<void*>(widget+0x1c0),&three,8);
        close_state=3;
    }
    return result;
}

// ---- The glyph list ------------------------------------------------------------------------
// A text object's string (+8, 0x30 bytes): data/heap block +0x10, capacity +0x28, allocator +0x30.
void free_text(unsigned char* text) {
    U cap=0,alloc=0,data=0;
    std::memcpy(&cap,text+0x28,8); std::memcpy(&alloc,text+0x30,8); std::memcpy(&data,text+0x10,8);
    if(cap>=8 && alloc) reinterpret_cast<Free>(*reinterpret_cast<U*>(*reinterpret_cast<U*>(alloc)+0x70))(alloc,data);
}
// One item through the game's add-item: name (FMG 200) and help (FMG 201) text objects whose view
// pointers are ours, and a std::function holding the game's function-pointer callable with our stub.
U add_one(U list,const Item& item) {
    alignas(16) unsigned char texts[0x80]={};
    reinterpret_cast<Text>(base+kText)(texts,200,kOfflineNotice);
    reinterpret_cast<Text>(base+kText)(texts+0x40,201,kOfflineNotice);
    const U name=reinterpret_cast<U>(item.label), help=reinterpret_cast<U>(item.help);
    std::memcpy(texts,&name,8); std::memcpy(texts+0x40,&help,8);
    struct alignas(16) { U vptr; U fn; U pad[2]; U impl; } function{};
    function.vptr=image()+kFnVtable; function.fn=item.stub; function.impl=reinterpret_cast<U>(&function);
    list=original_add_item(list,reinterpret_cast<U>(texts),reinterpret_cast<U>(&function),0,0,0);
    reinterpret_cast<Free>(*reinterpret_cast<U*>(*reinterpret_cast<U*>(function.impl)+0x20))(
        function.impl,function.impl!=reinterpret_cast<U>(&function));
    free_text(texts); free_text(texts+0x40);
    return list;
}
// The altar-page builder adds its three items here: while one of our pages is being built, the
// first call adds our items instead and the other two add nothing (the builder's greyed flags then
// land on our last item, enabled offline by both patches).
U SYSV add_item_hook(U list,U texts,U function,U d,U e,U f) {
    if(!building) return original_add_item(list,texts,function,d,e,f);
    if(++building_calls>1) return list;
    for(size_t i=0;i<building_count;++i) list=add_one(list,building[i]);
    return list;
}
// A page of the given items, built by the game's altar-page builder (title "Ritual Altar").
U build_page(U out,U context,const Item* items,size_t count) {
    building=items; building_count=count; building_calls=0;
    reinterpret_cast<U (SYSV *)(U,U)>(base+kAltarPage)(out,context);
    building=nullptr;
    return out;
}
// Offline "Chalice Dungeon Search": the first characters of the catalog's glyphs.
U SYSV search_hook(U out,U context,U c,U d,U e,U f) {
    if(!offline()) return original_search(out,context,c,d,e,f);
    altar_context=context;
    log_line("glyph list opened");
    // The groups' help lines, once, in the game's language (before their page exists).
    static bool group_helps_done=false;
    U repository=0;
    if(!group_helps_done && peek(image()+kMsgRepository,repository) && repository) {
        const lc_desc::Language& lang=language();
        for(size_t i=0;i<groups.size() && i<group_items.size();++i) {
            if(static_cast<size_t>(kGroupHelp)<lang.messages.size())
                groups[i].help=fill(lang.messages[kGroupHelp],"",groups[i].first);
            group_items[i].help=groups[i].help.c_str();
        }
        group_helps_done=true;
    }
    return build_page(out,context,group_items.data(),group_items.size());
}
// The game's loaded text for (fmg, id), or null.
const char16_t* game_text(void*,uint32_t fmg,uint32_t id) {
    U repository=0;
    if(!peek(image()+kMsgRepository,repository) || !repository) return nullptr;
    return reinterpret_cast<const char16_t* (SYSV *)(U,U,U,U)>(base+kMsgLookup)(repository,0,fmg,id);
}
// The descriptions of glyphs [begin, begin+count), in the game's language (game thread).
void compose_helps(size_t begin,size_t count) {
    U repository=0;
    if(!peek(image()+kMsgRepository,repository) || !repository) return; // text not loaded: keep placeholders
    const lc_desc::Language& lang=language();
    for(size_t i=begin;i<begin+count && i<catalog.size();++i) {
        if(desc_done[i]) continue;
        const Entry& e=catalog[i];
        glyph_helps[i]=lc_desc::compose(desc_items.data()+e.item_first,e.item_count,lang,game_text,nullptr);
        glyph_items[i].help=glyph_helps[i].c_str();
        desc_done[i]=true;
    }
}
// A first character: its glyphs, in alphabetical order.
U SYSV group_handler(U out,U context,U index) {
    if(index>=groups.size()) return out;
    group_context=context;
    compose_helps(groups[index].begin,groups[index].count);
    return build_page(out,context,&glyph_items[groups[index].begin],groups[index].count);
}
// A glyph: placed on the current altar as typed glyphs are; on success the list, its group page
// and the altar page close after the OK box.
U SYSV glyph_handler(U out,U context,U index) {
    if(index>=catalog.size()) return out;
    const std::string glyph(catalog[index].glyph,8);
    // The game calls an item's handler a second time right after the first (both in-game logs:
    // the second call found the altar occupied). That call is answered with the first one's
    // message; it changes nothing and does not arm the menu close again.
    static size_t last_index=~size_t(0);
    static std::chrono::steady_clock::time_point last_time;
    const auto now=std::chrono::steady_clock::now();
    if(index==last_index && now-last_time<std::chrono::seconds(3)) {
        log_line("the same glyph again at once: answered with the placement's message");
        return message_box(out,context,say(language(),{kPlaced,glyph}));
    }
    bool placed=false;
    const Message m=place(glyph,placed);
    log_line(ascii(say(desc_languages.front(),m)).c_str());
    const std::u16string text=say(language(),m);
    if(!placed) { last_index=~size_t(0); return message_box(out,context,text); }
    last_index=index; last_time=now;
    const U parents[2]={group_context,altar_context};
    return message_then_close(out,context,text,parents,2);
}
// Per-item stubs: movabs $index,%rdx; movabs $handler,%rax; jmp *%rax (rdi/rsi kept).
bool make_stubs() {
    const size_t n=groups.size()+catalog.size();
    unsigned char* code=lc_platform::alloc_code(n*32);
    if(!code) return false;
    auto emit=[&](size_t slot,U index,U handler) {
        unsigned char* p=code+slot*32;
        p[0]=0x48; p[1]=0xba; std::memcpy(p+2,&index,8);
        p[10]=0x48; p[11]=0xb8; std::memcpy(p+12,&handler,8);
        p[20]=0xff; p[21]=0xe0;
        return reinterpret_cast<U>(p);
    };
    group_items.clear(); glyph_items.clear();
    for(size_t i=0;i<groups.size();++i)
        group_items.push_back({groups[i].label.c_str(),groups[i].help.c_str(),
                               emit(i,i,reinterpret_cast<U>(&group_handler))});
    for(size_t i=0;i<catalog.size();++i)
        glyph_items.push_back({glyph_labels[i].c_str(),glyph_helps[i].c_str(),
                               emit(groups.size()+i,i,reinterpret_cast<U>(&glyph_handler))});
    return lc_platform::seal_code(code,n*32);
}

U SYSV idle_hook(U helper) {
    if(helper==watch_helper.load()) {
        uint32_t request=1;
        if(peek(helper+0x260,request) && !request) idle_seen.store(true);
    }
    return original_idle(helper);
}
// The glyph flow after the player confirmed "Search using Chalice Glyph: <glyph>" (rdi = result
// action, rsi = the lambda: +8 the flow's shared state).
U SYSV continuation_hook(U out,U lambda,U c,U d,U e,U f) {
    U state=0,context=0;
    std::string glyph;
    if(!offline() || !peek(lambda+8,state) || !state || !peek(state,context) || !context ||
       !flow_glyph(state,glyph))
        return original_continuation(out,lambda,c,d,e,f);
    Message m{kInvalid,glyph};
    bool placed=false;
    if(glyph.size()==8 && std::all_of(glyph.begin(),glyph.end(),[](char ch){ return std::isalnum(static_cast<unsigned char>(ch)); }))
        m=place(glyph,placed);
    log_line(ascii(say(desc_languages.front(),m)).c_str());
    const std::u16string text=say(language(),m);
    return placed ? message_then_close(out,context,text) : message_box(out,context,text);
}
void jump(unsigned char* p,U target) {
    // jmp qword ptr [rip+0]; <target>. Does not destroy registers or flags.
    const unsigned char op[]={0xff,0x25,0,0,0,0};
    std::memcpy(p,op,6); std::memcpy(p+6,&target,8);
}
// On by default (installing the mod is the choice); BB_LOCAL_CHALICE_NATIVE=0 turns it off.
// Never in a launch the port starts online (BB_ONLINE=1) or under BBSC.
// Also not in a fork's party co-op launch (BB_PARTY on: visy's port, Nightwatch Co-op), where
// other players share the world; BB_LOCAL_CHALICE_NATIVE=1 allows it there.
bool switched_on(const char* v) {
    if(!v || !*v) return false;
    std::string s(v);
    for(auto& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s!="0" && s!="off" && s!="false" && s!="no";
}
bool opt_in() {
    const char* flag=std::getenv("BB_LOCAL_CHALICE_NATIVE");
    const char* online=std::getenv("BB_ONLINE");
    if(flag && !std::strcmp(flag,"0")) { log_line("off (BB_LOCAL_CHALICE_NATIVE=0)"); return false; }
    if(online && !std::strcmp(online,"1")) { log_line("off in an online launch (BB_ONLINE=1)"); return false; }
    if(std::getenv("BBSC_EMULATOR")) { log_line("off under BBSC"); return false; }
    if(switched_on(std::getenv("BB_PARTY")) && !(flag && !std::strcmp(flag,"1"))) {
        log_line("off in a party co-op launch (BB_PARTY; BB_LOCAL_CHALICE_NATIVE=1 allows it)");
        return false;
    }
    return true;
}
std::u16string ascii16(const std::string& s) { return std::u16string(s.begin(),s.end()); }
// Catalog v5 (make_catalog_v4.py): sorted records, description recipes, probes, languages. Builds
// the menu labels and the first-character groups once.
bool load_catalog() {
    FILE* f=lc_platform::open_catalog();
    if(!f) return false;
    std::vector<unsigned char> d;
    unsigned char buffer[4096];
    for(size_t n; (n=std::fread(buffer,1,sizeof buffer,f))>0 && d.size()<(64u<<20);) d.insert(d.end(),buffer,buffer+n);
    std::fclose(f);
    size_t p=0;
    bool ok=d.size()>=12 && !std::memcmp(d.data(),"LCCAT005",8);
    p=8;
    auto u32=[&]() { uint32_t v=0; if(p+4>d.size()) { ok=false; return v; } std::memcpy(&v,&d[p],4); p+=4; return v; };
    auto str=[&]() {
        uint16_t n=0;
        if(p+2>d.size()) { ok=false; return std::u16string(); }
        std::memcpy(&n,&d[p],2); p+=2;
        if(p+n>d.size()) { ok=false; return std::u16string(); }
        const auto s=lc_desc::utf16(reinterpret_cast<const char*>(&d[p]),n); p+=n;
        return s;
    };
    const uint32_t count=u32();
    ok=ok && count>0 && count<100000 && p+size_t(count)*164<=d.size();
    if(ok) {
        catalog.resize(count);
        for(auto& e:catalog) {
            std::memcpy(e.glyph,&d[p],8); std::memcpy(&e.channel,&d[p+8],8); std::memcpy(e.form,&d[p+16],124);
            std::memcpy(e.name,&d[p+140],16); std::memcpy(&e.item_first,&d[p+156],4); std::memcpy(&e.item_count,&d[p+160],4);
            p+=164;
            ok=ok && e.form[0]==4 && e.form[1]==0x7c && (e.form[2]>>24)==0x1d;
        }
        const uint32_t items=u32();
        ok=ok && items<1000000 && p+size_t(items)*sizeof(lc_desc::Item)<=d.size();
        if(ok) {
            desc_items.resize(items);
            std::memcpy(desc_items.data(),&d[p],size_t(items)*sizeof(lc_desc::Item));
            p+=size_t(items)*sizeof(lc_desc::Item);
        }
        for(const auto& e:catalog) ok=ok && e.item_count<=64 && size_t(e.item_first)+e.item_count<=desc_items.size();
        const uint32_t probes=ok ? u32() : 0;
        ok=ok && probes<=16;
        desc_probes.clear();
        for(uint32_t i=0;ok && i<probes;++i) { const uint32_t fmg=u32(), id=u32(); desc_probes.push_back({fmg,id}); }
        const uint32_t languages=ok ? u32() : 0;
        ok=ok && languages>=1 && languages<=64;
        desc_languages.clear();
        for(uint32_t i=0;ok && i<languages;++i) {
            lc_desc::Language l;
            l.probe=u32();
            const std::u16string code=str();
            l.code.assign(code.begin(),code.end());
            l.comma=str(); l.farm=str(); l.rare=str(); l.glitch=str(); l.false_depth=str();
            uint16_t messages=0;
            if(p+2<=d.size()) { std::memcpy(&messages,&d[p],2); p+=2; } else ok=false;
            for(uint16_t m=0;ok && m<messages;++m) l.messages.push_back(str());
            ok=ok && l.messages.size()>=kMessages;
            desc_languages.push_back(l);
        }
    }
    if(!ok) { catalog.clear(); desc_items.clear(); desc_languages.clear(); return false; }
    glyph_labels.clear(); glyph_helps.clear(); groups.clear();
    glyph_labels.reserve(count); glyph_helps.reserve(count);
    desc_done.assign(count,false); desc_language=nullptr;
    for(size_t i=0;i<catalog.size();++i) {
        const Entry& e=catalog[i];
        glyph_labels.push_back(ascii16(std::string(e.glyph,8)));
        glyph_helps.push_back(u"—");
        if(groups.empty() || groups.back().first!=e.glyph[0]) groups.push_back({e.glyph[0],i,0,{},{}});
        ++groups.back().count;
    }
    for(auto& g:groups) {
        g.label=ascii16(std::string(1,g.first)+"    ("+std::to_string(g.count)+")");
        g.help=fill(desc_languages.front().messages[kGroupHelp],"",g.first); // until the game's language is known
    }
    return true;
}
bool matches(const unsigned char* img,U at,const unsigned char* bytes,size_t n) {
    return !std::memcmp(img+at,bytes,n);
}
}

// Code offsets this module writes (jump = 14 bytes): for a loader that patches a running game
// (lc_dll.cpp), which must make sure no thread is executing there at that moment.
extern "C" const uintptr_t lc_patch_sites[] = {kWaitForRequest, kContinuation, kPageUpdate,
                                              kAddItem, kSearchHandler, kEnableByte, kEnable2Byte,
                                              kHandlerPatch+4, 0};
constexpr size_t kJumpSize = 14;

// Installing is split for a loader that patches a running game (lc_dll.cpp): lc_stage checks the
// image, loads the catalog and builds the trampolines (allocates, opens files, logs: other threads
// must be running); lc_commit only writes the game's code (no allocation, no locks: run it with the
// other threads held); lc_report logs the outcome afterwards.
static unsigned char* staged=nullptr;
extern "C" int lc_stage(unsigned char* img,size_t size) {
    if(!opt_in()) return 0;
    if(size<0x553e9f0 ||
       !matches(img,kWaitForRequest,idle_head,sizeof idle_head) ||
       !matches(img,kContinuation,cont_head,sizeof cont_head) ||
       !matches(img,kGetFlag+kBodyAt,get_body,sizeof get_body) ||
       !matches(img,kSetFlag+kBodyAt,set_body,sizeof set_body) ||
       !matches(img,kResume+kBodyAt,resume_body,sizeof resume_body) ||
       !matches(img,kText+kBodyAt,text_body,sizeof text_body) ||
       !matches(img,kMsgLookup+kBodyAt,lookup_body,sizeof lookup_body) ||
       // kText itself: lea kMsgRepository(%rip),%r15 at +0x31 ... call kMsgLookup at +0x6b.
       !matches(img,kText+0x31,lea_r15,sizeof lea_r15) ||
       *reinterpret_cast<const int32_t*>(img+kText+0x34)!=static_cast<int32_t>(kMsgRepository-(kText+0x38)) ||
       img[kText+0x6b]!=0xe8 ||
       *reinterpret_cast<const int32_t*>(img+kText+0x6c)!=static_cast<int32_t>(kMsgLookup-(kText+0x70)) ||
       !matches(img,kOkBox+kBodyAt,box_body,sizeof box_body) ||
       !matches(img,kPageUpdate,page_head,sizeof page_head) ||
       !matches(img,kEnablePatch,enable_before,sizeof enable_before) ||
       !matches(img,0x1b667ac,enable_target,sizeof enable_target) ||
       !matches(img,kHandlerPatch,handler_before,sizeof handler_before) ||
       !matches(img,kAddItem,add_head,sizeof add_head) ||
       !matches(img,kSearchHandler,search_head,sizeof search_head) ||
       !matches(img,kAltarPage+kBodyAt,altar_body,sizeof altar_body) ||
       !matches(img,kEnable2Patch,enable2_before,sizeof enable2_before) ||
       !matches(img,0x1b6666f,enable2_target,sizeof enable2_target) ||
       *reinterpret_cast<const U*>(img+kFnVtable+0x10)!=reinterpret_cast<U>(img+kFnCall)) {
        log_line("game code does not match 1.09 (or another mod changed it); disabled");
        return 0;
    }
    base=img;
    if(!load_catalog()) { log_line("catalog missing or invalid (LocalChalice\\catalog.bin next to the mod); disabled"); return 0; }
    if(!make_stubs()) { log_line("could not prepare the glyph list; disabled"); return 0; }
    // Trampolines: copied prologue + jmp back.
    struct { U at; const unsigned char* head; size_t size; } const sites[]={
        {kWaitForRequest,idle_head,sizeof idle_head}, {kContinuation,cont_head,sizeof cont_head},
        {kPageUpdate,page_head,sizeof page_head}, {kAddItem,add_head,sizeof add_head},
        {kSearchHandler,search_head,sizeof search_head}};
    constexpr size_t kSites=sizeof sites/sizeof *sites;
    unsigned char* stub=lc_platform::alloc_code(64*kSites);
    if(!stub) return 0;
    for(size_t i=0;i<kSites;++i) {
        std::memcpy(stub+64*i,sites[i].head,sites[i].size);
        jump(stub+64*i+sites[i].size,reinterpret_cast<U>(img+sites[i].at+sites[i].size));
    }
    if(!lc_platform::seal_code(stub,64*kSites)) return 0;
    original_idle=reinterpret_cast<Step>(stub);
    original_continuation=reinterpret_cast<Call6>(stub+64);
    original_page_update=reinterpret_cast<Call6>(stub+128);
    original_add_item=reinterpret_cast<Call6>(stub+192);
    original_search=reinterpret_cast<Call6>(stub+256);
    staged=img;
    return 1;
}
extern "C" int lc_commit() {
    unsigned char* img=staged;
    if(!img) return 0;
    // Every write goes through write_code (VirtualProtect / mprotect / the host's patch; no heap,
    // no CRT locks). The hooks first (each passes through while it has nothing to do), the menu
    // enables last: if a write fails, the items stay greyed as in the game.
    const unsigned char enable=0x19, nops[2]={0x90,0x90};
    unsigned char hook[kJumpSize];
    bool ok=true;
    const struct { U at; U target; } entries[]={
        {kWaitForRequest,reinterpret_cast<U>(&idle_hook)},
        {kContinuation,reinterpret_cast<U>(&continuation_hook)},
        {kPageUpdate,reinterpret_cast<U>(&page_update_hook)},
        {kAddItem,reinterpret_cast<U>(&add_item_hook)},
        {kSearchHandler,reinterpret_cast<U>(&search_hook)}};
    for(const auto& entry:entries) {
        jump(hook,entry.target);
        ok=ok && write_code(img+entry.at,hook,kJumpSize);
    }
    ok=ok && write_code(img+kHandlerPatch+4,nops,2) && write_code(img+kEnableByte,&enable,1) &&
       write_code(img+kEnable2Byte,&enable,1);
    staged=nullptr;
    return ok;
}
// For a host with its own log and code writer (lc_bbhost.cpp); call before lc_install.
extern "C" void lc_set_host(void (*log)(const char*),bool (*write)(void*,const void*,size_t)) {
    host_log=log; host_write=write;
}
extern "C" void lc_report(int ok) {
    char line[96];
    std::snprintf(line,sizeof line,ok?"Search by Chalice Glyph enabled offline (%zu glyphs)"
                                     :"could not write the game's code; partially installed (%zu glyphs)",
                  catalog.size());
    log_line(line);
}
// Both steps at once: for the port's loader, before the game runs (no other thread in the game).
extern "C" int lc_install(unsigned char* img,size_t size) {
    if(!lc_stage(img,size)) return 0;
    const int ok=lc_commit();
    lc_report(ok);
    return ok;
}

// Called by the port after loading and patching the image, before the game runs (probe.c).
// The process claim keeps a copy-into-bin build of the same mod (pdh.dll / winmm.dll) from
// installing it a second time.
extern "C" void lc_prepare(unsigned char* img,size_t size) {
    if(!lc_platform::claim_process()) { log_line("another LocalChalice loader of this process has it"); return; }
    lc_install(img,size);
}
