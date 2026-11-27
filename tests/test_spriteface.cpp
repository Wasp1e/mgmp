#include "mgmp_spriteface.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

alignas(16) static uint8_t ch[0xF00], anim[0x300];
static int calls;
template<class T> void put(uint8_t* p, size_t off, T value) { std::memcpy(p+off,&value,sizeof(value)); }
static void str(uint8_t* p, size_t off, const char* value) {
    std::memset(p+off,0,32);
    std::memcpy(p+off,value,std::strlen(value));
    put<size_t>(p,off+16,std::strlen(value)); put<size_t>(p,off+24,15);
}
static void __fastcall native(void* actor, uint64_t dir, unsigned char a, unsigned char b) {
    if (actor != ch || a || b) calls += 100;
    ++calls;
    std::memcpy(ch+0x388,&dir,8); std::memcpy(ch+0x390,&dir,8);
    put<int>(anim,0x14C,-1); str(anim,0xB8,"B");
}
static void setup() {
    std::memset(ch,0,sizeof(ch)); std::memset(anim,0,sizeof(anim)); calls=0;
    ch[0x11]=1; put<uint8_t*>(ch,0x58,anim);
    put<int>(ch,0x38C,1); put<int>(ch,0x390,1); put<int>(ch,0x394,1);
    put<int>(ch,0x3A0,1); put<int>(ch,0x3A4,1);
    put<int>(anim,0x14C,-1); str(ch,0x3E0,""); str(anim,0xB8,"F");
}
int main() {
    int failed=0;
    auto check=[&](bool ok,const char* name){if(!ok){std::printf("FAIL: %s\n",name);++failed;}};
    setup(); uint8_t before[sizeof(ch)]; std::memcpy(before,ch,sizeof(ch));
    check(mgmp::refresh_sprite_facing(ch,native) && calls==1,"refresh stale F sprite for downward combat facing");
    check(!std::memcmp(before,ch,sizeof(ch)),"preserve all Character bytes including diagonal pending target");
    check(!mgmp::refresh_sprite_facing(ch,native) && calls==1,"idempotent when already correct");
    const size_t exclusions[]={0x4C2,0x4CC,0x4CD,0x3AC,0x3B4};
    for(auto off:exclusions){setup();ch[off]=1;check(!mgmp::refresh_sprite_facing(ch,native)&&calls==0,"skip dead/special/animated/locked characters");}
    setup(); anim[0x210]=1;
    check(!mgmp::refresh_sprite_facing(ch,native)&&calls==0,"skip pending turn animation");
    setup(); put<int>(ch,0x388,1);
    check(!mgmp::refresh_sprite_facing(ch,native)&&calls==0,"skip noncardinal combat direction");
    setup(); check(!mgmp::refresh_sprite_facing(ch,nullptr),"missing native function");
    // A busy sprite at the turn boundary must be retried once it settles.
    setup(); str(anim,0x108,"idle"); anim[0x210]=1;
    check(!mgmp::refresh_sprite_facing(ch,native,true)&&calls==0,"defer unfinished turn");
    anim[0x210]=0; str(anim,0x108,"walk");
    check(!mgmp::refresh_sprite_facing(ch,native,true)&&calls==0,"defer walking pose");
    str(anim,0x108,"attack");
    check(!mgmp::refresh_sprite_facing(ch,native,true)&&calls==0,"defer attack pose");
    str(anim,0x108,"idle"); std::memcpy(before,ch,sizeof(ch));
    check(mgmp::refresh_sprite_facing(ch,native,true)&&calls==1,"retry when idle without another turn boundary");
    check(!std::memcmp(before,ch,sizeof(ch)),"idle retry preserves combat state");
    check(!mgmp::refresh_sprite_facing(ch,native,true)&&calls==1,"idle retry remains idempotent");
    return failed?1:0;
}
