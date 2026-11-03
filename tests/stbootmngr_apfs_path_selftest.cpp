#define __STDC_HOSTED__ 0
#include "boot/x64/stbootmngr.efi/src/apfs_path_reader.hpp"

namespace {
constexpr uint32_t BS = 4096;
#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t BLOCKS = 2048;
static_assert(BS * BLOCKS <= 8ULL * 1024ULL * 1024ULL,
              "Phase 12 APFS fixture exceeds the 8 MiB test capacity");
#else
#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t BLOCKS = 1024;
static_assert(BS * BLOCKS <= 4ULL * 1024ULL * 1024ULL,
              "Phase 12 APFS fixture exceeds the 4 MiB test capacity");
#else
constexpr uint64_t BLOCKS = 16;
#endif
#endif
static uint8_t Image[BS * BLOCKS];

static void Put16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void Put32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void Put64(uint8_t* p, uint64_t v) { Put32(p,(uint32_t)v); Put32(p+4,(uint32_t)(v>>32)); }
static uint32_t Get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t Fletcher(const uint8_t* b, uint32_t n) {
    constexpr uint64_t M = 0xFFFFFFFFULL;
    uint64_t s1=0, s2=0;
    for (uint32_t i=8; i<n; i+=4) { s1=(s1+Get32(b+i))%M; s2=(s2+s1)%M; }
    const uint64_t lo=M-((s1+s2)%M);
    const uint64_t hi=M-((s1+lo)%M);
    return (hi<<32)|lo;
}
static void Seal(uint8_t* b) { Put64(b,Fletcher(b,BS)); }
static void ObjectHeader(uint8_t* b, uint64_t oid, uint64_t xid, uint32_t type, uint32_t subtype=0) {
    Put64(b+8,oid); Put64(b+16,xid); Put32(b+24,type); Put32(b+28,subtype);
}
static void OmapBlock(uint64_t block, uint64_t treePaddr, uint64_t xid=100) {
    uint8_t* b=Image+block*BS;
    ObjectHeader(b,block,xid,0x0000000BU);
    Put32(b+40,0x40000002U);
    Put64(b+48,treePaddr);
    Seal(b);
}
static void OmapLeaf(uint64_t block, uint64_t oid, uint64_t xid, uint64_t paddr,
                     uint32_t size=BS, uint32_t flags=0, bool root=true) {
    uint8_t* b=Image+block*BS;
    const uint16_t nodeFlags = (uint16_t)(0x0002U | 0x0004U | (root ? 0x0001U : 0));
    Put16(b+32,nodeFlags); Put16(b+34,0); Put32(b+36,1);
    Put16(b+40,0); Put16(b+42,4);
    Put16(b+56,0); Put16(b+58,16);
    Put64(b+60,oid); Put64(b+68,xid);
    const uint32_t valEnd = BS - (root ? 40U : 0U);
    const uint32_t valStart = valEnd - 16;
    Put32(b+valStart,flags); Put32(b+valStart+4,size); Put64(b+valStart+8,paddr);
    ObjectHeader(b,block,100,root ? 0x40000002U : 0x00000003U,0x0BU);
    if (root) { Put32(b+BS-40+4,BS); Put32(b+BS-40+8,16); Put32(b+BS-40+12,16); }
    Seal(b);
}
static void OmapBranchRoot(uint64_t block, uint64_t oid, uint64_t minXid, uint64_t childBlock) {
    uint8_t* b=Image+block*BS;
    Put16(b+32,0x0001U|0x0004U); Put16(b+34,1); Put32(b+36,1);
    Put16(b+40,0); Put16(b+42,4);
    Put16(b+56,0); Put16(b+58,8);
    Put64(b+60,oid); Put64(b+68,minXid);
    Put64(b+BS-40-8,childBlock);
    ObjectHeader(b,block,100,0x40000002U,0x0BU);
    Put32(b+BS-40+4,BS); Put32(b+BS-40+8,16); Put32(b+BS-40+12,16);
    Seal(b);
}
static EFI_STATUS ReadImage(void* ctx,uint64_t off,UINTN len,void* out) {
    auto* b=static_cast<uint8_t*>(ctx);
    if (!b || !out || off>sizeof(Image) || (uint64_t)len>sizeof(Image)-off) return EFI_LOAD_ERROR;
    auto* d=static_cast<uint8_t*>(out); for (UINTN i=0;i<len;++i) d[i]=b[off+i];
    return EFI_SUCCESS;
}
struct DrecFixture { uint64_t Parent; uint64_t FileId; const char* Name; uint16_t Flags; bool Hashed; };
static void BuildOmapLeafMany() {
    uint8_t* b=Image+10*BS;
    const uint64_t oids[] = {55,70,71};
    const uint64_t paddrs[] = {6,8,9};
    constexpr uint32_t count=3;
    Put16(b+32,0x0002U|0x0004U); Put16(b+34,0); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,count*4);
    for (uint32_t i=0;i<count;++i) {
        const uint32_t toc=56+i*4;
        Put16(b+toc,(uint16_t)(i*16));
        Put16(b+toc+2,(uint16_t)((i+1)*16));
        Put64(b+56+count*4+i*16,oids[i]);
        Put64(b+56+count*4+i*16+8,0);
        const uint32_t valueStart=BS-(i+1)*16;
        Put32(b+valueStart,0); Put32(b+valueStart+4,BS); Put64(b+valueStart+8,paddrs[i]);
    }
    ObjectHeader(b,10,100,0x00000003U,0x0BU);
    Put16(b+32,0x0002U|0x0004U); Put16(b+34,0); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,count*4);
    Seal(b);
}
static uint16_t MakeDrecKey(uint8_t* key, uint64_t parent, const char* name, bool hashed) {
    uint16_t nameLen=0;
    while (name[nameLen]) ++nameLen;
    ++nameLen;
    Put64(key, (9ULL<<60) | parent);
    uint16_t keySize=0;
    if (hashed) {
        const uint32_t hash=0x15555U;
        Put32(key+8, (hash<<11) | nameLen);
        for (uint16_t i=0;i<nameLen-1;++i) key[12+i]=(uint8_t)name[i];
        key[12+nameLen-1]=0;
        keySize=(uint16_t)(12+nameLen);
    } else {
        Put16(key+8,nameLen);
        for (uint16_t i=0;i<nameLen-1;++i) key[10+i]=(uint8_t)name[i];
        key[10+nameLen-1]=0;
        keySize=(uint16_t)(10+nameLen);
    }
    return keySize;
}
static void BuildDrecLeaf(uint64_t blockNo, const DrecFixture* records, uint32_t count, bool root) {
    uint8_t* b=Image+blockNo*BS;
    const uint32_t tocStart=56, tocLen=count*8, keyStart=tocStart+tocLen;
    const uint32_t valueEnd=BS-(root?40U:0U);
    Put16(b+32,(uint16_t)(0x0002U|(root?0x0001U:0))); Put16(b+34,0); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,tocLen);
    uint32_t keyPos=0;
    for (uint32_t i=0;i<count;++i) {
        uint8_t key[300]{};
        const uint16_t keyLen=MakeDrecKey(key,records[i].Parent,records[i].Name,records[i].Hashed);
        const uint32_t entryOff=tocStart+i*8;
        const uint16_t valueOff=(uint16_t)((i+1)*18);
        Put16(b+entryOff,(uint16_t)keyPos); Put16(b+entryOff+2,keyLen);
        Put16(b+entryOff+4,valueOff); Put16(b+entryOff+6,18);
        for (uint16_t j=0;j<keyLen;++j) b[keyStart+keyPos+j]=key[j];
        keyPos+=keyLen;
        const uint32_t valueStart=valueEnd-valueOff;
        Put64(b+valueStart,records[i].FileId); Put64(b+valueStart+8,1000+i);
        Put16(b+valueStart+16,records[i].Flags);
    }
    ObjectHeader(b,blockNo==6?55:blockNo+60,100,root?0x40000002U:0x00000003U,0x0EU);
    Put16(b+32,(uint16_t)(0x0002U|(root?0x0001U:0))); Put16(b+34,0); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,tocLen);
    Seal(b);
}
static void BuildFsBranchRoot() {
    uint8_t* b=Image+6*BS;
    const char* names[] = {"System","CoreServices"};
    const uint64_t parents[] = {2,11};
    const uint64_t childOids[] = {70,71};
    constexpr uint32_t count=2;
    const uint32_t tocStart=56, tocLen=count*8, keyStart=tocStart+tocLen, valueEnd=BS-40;
    Put16(b+32,0x0001U); Put16(b+34,1); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,tocLen);
    uint32_t keyPos=0;
    for (uint32_t i=0;i<count;++i) {
        uint8_t key[300]{};
        const uint16_t keyLen=MakeDrecKey(key,parents[i],names[i],true);
        const uint32_t entryOff=tocStart+i*8;
        const uint16_t valueOff=(uint16_t)((i+1)*8);
        Put16(b+entryOff,(uint16_t)keyPos); Put16(b+entryOff+2,keyLen);
        Put16(b+entryOff+4,valueOff); Put16(b+entryOff+6,8);
        for (uint16_t j=0;j<keyLen;++j) b[keyStart+keyPos+j]=key[j];
        keyPos+=keyLen;
        Put64(b+valueEnd-valueOff,childOids[i]);
    }
    ObjectHeader(b,55,100,0x40000002U,0x0EU);
    Put16(b+32,0x0001U); Put16(b+34,1); Put32(b+36,count);
    Put16(b+40,0); Put16(b+42,tocLen);
    Seal(b);
}
static void BuildImage(bool branchTree) {
    for (uint64_t i=0;i<sizeof(Image);++i) Image[i]=0;
    uint8_t* nx=Image;
    ObjectHeader(nx,1,100,0x80000001U);
    Put32(nx+32,0x4253584EU); Put32(nx+36,BS); Put64(nx+40,BLOCKS);
    Put64(nx+160,1); Put32(nx+180,1); Put64(nx+184,42);
    Seal(nx);
    OmapBlock(1,2);
    OmapBranchRoot(2,42,0,7);
    OmapLeaf(7,42,100,3,BS,0,false);
    uint8_t* vol=Image+3*BS;
    ObjectHeader(vol,42,100,0x0000000DU); Put32(vol+32,0x42535041U);
    Put32(vol+116,0x4000000EU); Put64(vol+128,4); Put64(vol+136,55); Seal(vol);
    OmapBlock(4,5);
    if (branchTree) {
        OmapBranchRoot(5,55,0,10);
        BuildOmapLeafMany();
        const DrecFixture left[] = {{2,10,"System",4,true},{10,11,"Library",4,false}};
        const DrecFixture right[] = {{11,12,"CoreServices",4,true},{12,13,"boot.efi",8,true}};
        BuildFsBranchRoot();
        BuildDrecLeaf(8,left,2,false);
        BuildDrecLeaf(9,right,2,false);
    } else {
        OmapLeaf(5,55,100,6,BS,0,true);
        const DrecFixture records[] = {
            {2,10,"System",4,true}, {10,11,"Library",4,false},
            {11,12,"CoreServices",4,true}, {12,13,"boot.efi",8,true},
        };
        BuildDrecLeaf(6,records,4,true);
    }
}
} // namespace

int main() {
    for (uint32_t scenario=0; scenario<2; ++scenario) {
        BuildImage(scenario==0);
        APFS_SOURCE src{ReadImage,Image,sizeof(Image)};
        APFS_VOLUME_CONTEXT vol{};
        if (ApfsOpenVolume(&src,0,&vol)!=EFI_SUCCESS) return 1;

        APFS_PATH_RESULT result{};
        if (ApfsLookupPath(&vol,"/System/Library/CoreServices/boot.efi",&result)!=EFI_SUCCESS) return 2;
        if (result.FileId!=13 || result.IsDirectory || (result.DirectoryEntryFlags & 0xF)!=8) return 3;
        if (ApfsLookupPath(&vol,"/System/Library",&result)!=EFI_SUCCESS || result.FileId!=11 || !result.IsDirectory) return 4;
        if (ApfsLookupPath(&vol,"/Missing",&result)!=EFI_NOT_FOUND) return 5;
        if (ApfsLookupPath(&vol,"/System/Library/boot.efi",&result)!=EFI_NOT_FOUND) return 6;
        if (ApfsLookupPath(&vol,"/System/../boot.efi",&result)!=EFI_INVALID_PARAMETER) return 7;
        if (ApfsLookupPath(&vol,"/System//Library",&result)!=EFI_INVALID_PARAMETER) return 8;

        Image[6*BS] ^= 0x01;
        if (ApfsLookupPath(&vol,"/System",&result)==EFI_SUCCESS) return 9;
        Image[6*BS] ^= 0x01;
    }
    return 0;
}
