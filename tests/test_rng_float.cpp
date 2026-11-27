#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cmath>
#include "mgmp_record.h"

extern "C" {
void* mgmp_rng_native_randfloat = nullptr;
double mgmp_rng_randfloat(double, uint64_t*);
void rng_float_probe(void*, uint64_t*, void*, double);
void rng_float_clobber();
}
static unsigned observed;
static uintptr_t caller_seen;
static uint64_t s0_seen, result_seen;
static const void* state_seen;
extern "C" void mgmp_rng_observe_float(uintptr_t caller, uint64_t s0,
                                      uint64_t result, const void* state) {
    ++observed; caller_seen=caller; s0_seen=s0; result_seen=result; state_seen=state;
    rng_float_clobber();
}

// Reproduce the former C++ hook with a recorder that legally clobbers XMM2..5.
static double old_hook(double scale, uint64_t* state) {
    const auto original = (double (*)(double,uint64_t*))mgmp_rng_native_randfloat;
    const double value = original(scale,state);
    rng_float_clobber();
    return value;
}

static double old_record_hook(double scale, uint64_t* state) {
    const auto original = (double (*)(double,uint64_t*))mgmp_rng_native_randfloat;
    const uint64_t s0=state[0];
    const double value=original(scale,state);
    uint64_t bits=0; std::memcpy(&bits,&value,8);
    mgmp::record_rng(1,0x94F5B2,s0,bits,true,state);
    return value;
}

int main() {
    const char* path=std::getenv("MEWGENICS_EXE");
    if(!path) path="C:\\Program Files (x86)\\Steam\\steamapps\\common\\Mewgenics\\Mewgenics.exe";
    if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIP: native randfloat regression needs MEWGENICS_EXE\n"); return 0;
    }
    HMODULE mapping=LoadLibraryExA(path,nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!mapping) return 1;
    const auto* base=(const uint8_t*)((uintptr_t)mapping & ~(uintptr_t)3);
    // Independently checked leaf in the installed PE. No imports or calls.
    constexpr uint32_t rva=0x1595A0, size=0x94, constant=0x113F530;
    const uint8_t prefix[]={0x48,0x89,0x5C,0x24,0x08,0x48,0x8B,0x4A,0x08};
    if(std::memcmp(base+rva,prefix,sizeof(prefix))) return 2;
    auto* code=(uint8_t*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!code) return 3;
    std::memcpy(code,base+rva,size);
    std::memcpy(code+0x200,base+constant,8);
    unsigned patched=0;
    const uint8_t op[]={0xF2,0x0F,0x59,0x05};
    for(unsigned i=0;i+8<=size;++i) if(!std::memcmp(code+i,op,4)) {
        int32_t disp=0; std::memcpy(&disp,code+i+4,4);
        if((int64_t)rva+i+8+disp != constant) return 4;
        disp=0x200-(i+8); std::memcpy(code+i+4,&disp,4); ++patched;
    }
    if(patched!=2) return 5;
    DWORD previous=0;
    if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous)) return 6;
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    mgmp_rng_native_randfloat=code;
    unsigned trials=0;
    for(uint64_t n=1;n<=64;++n) for(double scale:{0.0,0.5,1.0,10.0}) {
        uint64_t a[4]={n,n*17,n*101,n*999}, b[4]; std::memcpy(b,a,sizeof(a));
        uint8_t expected[96]{}, actual[96]{};
        rng_float_probe(code,a,expected,scale);
        rng_float_probe((void*)&mgmp_rng_randfloat,b,actual,scale);
        uint64_t bits=0; std::memcpy(&bits,expected,8);
        if(std::memcmp(a,b,sizeof(a)) || std::memcmp(expected,actual,sizeof(actual)) ||
            s0_seen!=n || result_seen!=bits || state_seen!=b || !caller_seen) return 7;
        ++trials;
    }
    if(observed!=trials) return 9;
    // Ensure this regression really detects the former hook's corruption.
    uint64_t a[4]={1,2,3,4},b[4]={1,2,3,4}; uint8_t expected[96]{}, broken[96]{};
    rng_float_probe(code,a,expected,1.0);
    rng_float_probe((void*)&old_hook,b,broken,1.0);
    if(!std::memcmp(expected+32,broken+32,64)) return 8;
    wchar_t dir[MAX_PATH]{}, file[MAX_PATH]{};
    GetTempPathW(MAX_PATH,dir);
    GetTempFileNameW(dir,L"rng",0,file);
    mgmp::record_init(file,0x1574000,"native RNG register regression");
    uint64_t real_a[4]={1,2,3,4}, real_b[4]={1,2,3,4};
    rng_float_probe(code,real_a,expected,1.0);
    rng_float_probe((void*)&old_record_hook,real_b,broken,1.0);
    std::printf("Real recorder XMM2..5 corruption reproduced: %s\n",
                std::memcmp(expected+32,broken+32,64)?"YES":"not observed on this machine");
    // Also run the actual native luck-adjusted sampler. Its leaf calls depend
    // on the register behavior under investigation, unlike a normal ABI caller.
    auto* image=(uint8_t*)VirtualAlloc(nullptr,0x1574000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!image) return 10;
    const auto* dos=(const IMAGE_DOS_HEADER*)base;
    const auto* nt=(const IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    std::memcpy(image,base,nt->OptionalHeader.SizeOfHeaders);
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        std::memcpy(image+section.VirtualAddress,base+section.VirtualAddress,section.SizeOfRawData);
    }
    auto jump=[](uint8_t* where,void* target) {
        where[0]=0x48; where[1]=0xB8; std::memcpy(where+2,&target,8);
        where[10]=0xFF; where[11]=0xE0;
    };
    jump(image+0xD5FA70,(void*)(double(*)(double,double))&std::pow);
    auto sampler=(double(*)(double,uint64_t*))(image+0x94F4C0);
    auto protect=[&](DWORD mode) { return VirtualProtect(image,0x1574000,mode,&previous)!=0; };
    if(!protect(PAGE_EXECUTE_READ)) return 11;
    unsigned real_differences=0;
    for(double luck:{-2.5,-0.25,0.0,0.5,1.0,2.75}) {
        uint64_t aa[4]={17,31,101,999},bb[4],cc[4];
        std::memcpy(bb,aa,32); std::memcpy(cc,aa,32);
        const double reference=sampler(luck,aa);
        protect(PAGE_READWRITE); jump(image+rva,(void*)&old_record_hook);
        protect(PAGE_EXECUTE_READ); FlushInstructionCache(GetCurrentProcess(),image,0x1574000);
        const double old_value=sampler(luck,bb);
        protect(PAGE_READWRITE); jump(image+rva,(void*)&mgmp_rng_randfloat);
        protect(PAGE_EXECUTE_READ); FlushInstructionCache(GetCurrentProcess(),image,0x1574000);
        const double new_value=sampler(luck,cc);
        if(std::memcmp(&reference,&new_value,8) || std::memcmp(aa,cc,32)) return 12;
        if(std::memcmp(&reference,&old_value,8) || std::memcmp(aa,bb,32)) ++real_differences;
        protect(PAGE_READWRITE); std::memcpy(image+rva,base+rva,size);
        protect(PAGE_EXECUTE_READ); FlushInstructionCache(GetCurrentProcess(),image,0x1574000);
    }
    std::printf("Actual luck sampler differences with real recorder: %u/6\n",real_differences);
    VirtualFree(image,0,MEM_RELEASE);
    mgmp::record_shutdown(); DeleteFileW(file);
    VirtualFree(code,0,MEM_RELEASE); FreeLibrary(mapping);
    std::printf("PASS: %u native RNG/state/register comparisons; clobbering-observer regression reproduced\n",trials);
    return 0;
}
