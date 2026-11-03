#define __STDC_HOSTED__ 0
#include "boot/x64/stbootmngr.efi/src/apfs_metadata_reader.hpp"

namespace {
constexpr uint32_t BS = 4096;
constexpr uint64_t BLOCKS = 8;
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
    Put32(b+40,0x40000002U); // om_tree_type: physical B-tree root
    Put64(b+48,treePaddr);
    Seal(b);
}
static void OmapLeaf(uint64_t block, uint64_t oid, uint64_t xid, uint64_t paddr,
                     uint32_t size=BS, uint32_t flags=0, bool root=true) {
    uint8_t* b=Image+block*BS;
    const uint16_t nodeFlags = (uint16_t)(0x0002U | 0x0004U | (root ? 0x0001U : 0));
    Put16(b+32,nodeFlags); Put16(b+34,0); Put32(b+36,1);
    Put16(b+40,0); Put16(b+42,4); // one fixed TOC entry
    Put16(b+56,0); Put16(b+58,16); // key offset 0, value is 16 bytes from end
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
    Put16(b+56,0); Put16(b+58,8); // reversed distance from value-area end to 8-byte child value
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
static void BuildImage() {
    for (uint64_t i=0;i<sizeof(Image);++i) Image[i]=0;
    uint8_t* nx=Image;
    ObjectHeader(nx,1,100,0x80000001U);
    Put32(nx+32,0x4253584EU); Put32(nx+36,BS); Put64(nx+40,BLOCKS);
    Put64(nx+160,1); Put32(nx+180,100); Put64(nx+184,42);
    Seal(nx);
    OmapBlock(1,2);
    OmapBranchRoot(2,42,0,7); // internal/root B-tree node points to block 7
    OmapLeaf(7,42,100,3,BS,0,false);
    uint8_t* vol=Image+3*BS;
    ObjectHeader(vol,42,100,0x0000000DU); Put32(vol+32,0x42535041U);
    Put32(vol+116,0x4000000EU); Put64(vol+128,4); Put64(vol+136,55); Seal(vol);
    OmapBlock(4,5);
    OmapLeaf(5,55,100,6,BS,0,true);
    uint8_t* fsroot=Image+6*BS;
    ObjectHeader(fsroot,55,100,0x40000002U,0x0EU);
    Put16(fsroot+32,0x0001U|0x0002U); Put32(fsroot+36,0); Seal(fsroot);
}
} // namespace

int main() {
    BuildImage();
    APFS_SOURCE src{ReadImage,Image,sizeof(Image)};
    APFS_VOLUME_CONTEXT vol{};
    if (ApfsOpenVolume(&src,0,&vol)!=EFI_SUCCESS) return 1;
    if (!vol.Opened || vol.BlockSize!=BS || vol.BlockCount!=BLOCKS ||
        vol.ContainerXid!=100 || vol.VolumeOid!=42 || vol.VolumePaddr!=3 ||
        vol.VolumeOmapPaddr!=4 || vol.RootTreeOid!=55 || vol.RootTreePaddr!=6) return 2;
    uint64_t paddr=0; uint32_t size=0;
    if (ApfsResolveContainerObject(&vol,42,100,&paddr,&size)!=EFI_SUCCESS || paddr!=3 || size!=BS) return 3;
    if (ApfsResolveVolumeObject(&vol,55,100,&paddr,&size)!=EFI_SUCCESS || paddr!=6 || size!=BS) return 4;
    if (ApfsResolveVolumeObject(&vol,999,100,&paddr,&size)==EFI_SUCCESS) return 5;

    // A corrupt APFS object checksum must fail closed before metadata is trusted.
    Image[0] ^= 0x01;
    APFS_VOLUME_CONTEXT bad{};
    if (ApfsOpenVolume(&src,0,&bad)==EFI_SUCCESS) return 6;
    Image[0] ^= 0x01;

    // Out-of-range object-map physical address must be rejected.
    Put64(Image+5*BS+BS-40-16+8, BLOCKS+1); // replace paddr inside leaf value
    Seal(Image+5*BS);
    APFS_VOLUME_CONTEXT badMap{};
    if (ApfsOpenVolume(&src,0,&badMap)==EFI_SUCCESS) return 7;
    return 0;
}
