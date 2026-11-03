#define __STDC_HOSTED__ 0
#include "boot/x64/stbootmngr.efi/src/udif_reader.hpp"
#include "boot/x64/stbootmngr.efi/src/udif_zlib_inflate.hpp"

namespace {
constexpr uint64_t kDmgBytes = 32768;
constexpr uint64_t kApfsBytes = 16384;
static uint8_t gDmg[kDmgBytes];
static uint8_t gApfsRaw[kApfsBytes];
static uint8_t gZlibStream[kApfsBytes + 32];
static uint8_t gMish[284];
static char gBase64[512];
static char gXml[4096];

static void Zero(void* Dst, uint64_t N) {
    auto* P = static_cast<uint8_t*>(Dst);
    for (uint64_t I = 0; I < N; ++I) P[I] = 0;
}
static void Copy(void* Dst, const void* Src, uint64_t N) {
    auto* D = static_cast<uint8_t*>(Dst);
    const auto* S = static_cast<const uint8_t*>(Src);
    for (uint64_t I = 0; I < N; ++I) D[I] = S[I];
}
static void Be32(uint8_t* P, uint32_t V) {
    P[0] = (uint8_t)(V >> 24); P[1] = (uint8_t)(V >> 16);
    P[2] = (uint8_t)(V >> 8); P[3] = (uint8_t)V;
}
static void Be64(uint8_t* P, uint64_t V) {
    Be32(P, (uint32_t)(V >> 32)); Be32(P + 4, (uint32_t)V);
}
static void Le32(uint8_t* P, uint32_t V) {
    P[0] = (uint8_t)V; P[1] = (uint8_t)(V >> 8);
    P[2] = (uint8_t)(V >> 16); P[3] = (uint8_t)(V >> 24);
}
static void AddText(char* Out, uint64_t* Pos, const char* Text) {
    for (uint64_t I = 0; Text[I]; ++I) Out[(*Pos)++] = Text[I];
    Out[*Pos] = 0;
}
static uint64_t Base64Encode(const uint8_t* In, uint64_t N, char* Out, uint64_t Capacity) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint64_t Pos = 0;
    for (uint64_t I = 0; I < N; I += 3) {
        uint32_t A = In[I];
        uint32_t B = (I + 1 < N) ? In[I + 1] : 0;
        uint32_t C = (I + 2 < N) ? In[I + 2] : 0;
        uint32_t X = (A << 16) | (B << 8) | C;
        if (Pos + 4 >= Capacity) return 0;
        Out[Pos++] = T[(X >> 18) & 63];
        Out[Pos++] = T[(X >> 12) & 63];
        Out[Pos++] = (I + 1 < N) ? T[(X >> 6) & 63] : '=';
        Out[Pos++] = (I + 2 < N) ? T[X & 63] : '=';
    }
    if (Pos < Capacity) Out[Pos] = 0;
    return Pos;
}
struct Blob { uint8_t* Data; uint64_t Size; };
static EFI_STATUS ReadBlob(void* Context, uint64_t Offset, UINTN Length, void* Buffer) {
    auto* B = static_cast<Blob*>(Context);
    if (!B || !Buffer || Offset > B->Size || (uint64_t)Length > B->Size - Offset)
        return EFI_LOAD_ERROR;
    Copy(Buffer, B->Data + Offset, Length);
    return EFI_SUCCESS;
}
static uint32_t Adler32(const uint8_t* Data, uint64_t Size) {
    uint32_t A = 1, B = 0;
    for (uint64_t I = 0; I < Size; ++I) {
        A = (A + Data[I]) % 65521U;
        B = (B + A) % 65521U;
    }
    return (B << 16) | A;
}
static uint64_t BuildZlibStored(const uint8_t* Raw, uint64_t RawSize,
                                uint8_t* Out, uint64_t Capacity) {
    if (!Raw || !Out || RawSize > 65535 || Capacity < RawSize + 11) return 0;
    Out[0] = 0x78; Out[1] = 0x01; // zlib CMF/FLG
    Out[2] = 0x01; // final DEFLATE block, stored type
    const uint16_t Len = (uint16_t)RawSize;
    const uint16_t NLen = (uint16_t)~Len;
    Out[3] = (uint8_t)Len; Out[4] = (uint8_t)(Len >> 8);
    Out[5] = (uint8_t)NLen; Out[6] = (uint8_t)(NLen >> 8);
    Copy(Out + 7, Raw, RawSize);
    const uint32_t Adler = Adler32(Raw, RawSize);
    Be32(Out + 7 + RawSize, Adler);
    return RawSize + 11;
}
static void BuildDmg(uint32_t RunType, bool CorruptAdler = false) {
    Zero(gDmg, sizeof(gDmg));
    Zero(gApfsRaw, sizeof(gApfsRaw));
    Zero(gZlibStream, sizeof(gZlibStream));
    Zero(gMish, sizeof(gMish));
    Zero(gBase64, sizeof(gBase64));
    Zero(gXml, sizeof(gXml));
    // Minimal test APFS NXSB at the beginning of the virtual APFS partition.
    Copy(gApfsRaw + 32, "NXSB", 4);
    Le32(gApfsRaw + 36, 4096);
    Le32(gApfsRaw + 40, 4); // low 32 bits of block count = 4
    Le32(gApfsRaw + 160, 7); // object-map OID low word
    Le32(gApfsRaw + 180, 1);
    Le32(gApfsRaw + 184, 9);

    const uint64_t DataForkLength = kApfsBytes + 64;
    uint64_t StoredLength = kApfsBytes;
    if (RunType == 0x80000005U) {
        StoredLength = BuildZlibStored(gApfsRaw, sizeof(gApfsRaw),
                                       gDmg, sizeof(gDmg));
        if (!StoredLength) return;
        if (CorruptAdler) gDmg[StoredLength - 1] ^= 0x80;
    } else {
        Copy(gDmg, gApfsRaw, sizeof(gApfsRaw));
    }

    Copy(gMish, "mish", 4);
    Be32(gMish + 4, 1);
    Be64(gMish + 8, 100); // APFS partition starts at virtual disk sector 100
    Be64(gMish + 16, 32);  // 16 KiB partition
    Be64(gMish + 24, 0);   // block data offset inside the data fork
    Be32(gMish + 36, 2);   // descriptor count
    Be32(gMish + 200, 2);  // chunk count
    Be32(gMish + 204, RunType);
    Be64(gMish + 204 + 8, 0);
    Be64(gMish + 204 + 16, 32);
    Be64(gMish + 204 + 24, 0);
    Be64(gMish + 204 + 32, StoredLength);
    Be32(gMish + 244, 0xFFFFFFFFU); // end chunk
    Base64Encode(gMish, sizeof(gMish), gBase64, sizeof(gBase64));

    uint64_t XmlPos = 0;
    AddText(gXml, &XmlPos, "<?xml version=\"1.0\"?><plist><dict><key>resource-fork</key><dict><key>blkx</key><array><dict><key>Name</key><string>disk image (Apple_APFS : 3)</string><key>Data</key><data>");
    AddText(gXml, &XmlPos, gBase64);
    AddText(gXml, &XmlPos, "</data></dict></array></dict></dict></plist>");

    const uint64_t XmlOffset = DataForkLength;
    Copy(gDmg + XmlOffset, gXml, XmlPos);
    uint8_t* Footer = gDmg + kDmgBytes - 512;
    Copy(Footer, "koly", 4);
    Be32(Footer + 4, 4);
    Be32(Footer + 8, 512);
    Be64(Footer + 24, 0);
    Be64(Footer + 32, DataForkLength);
    Be64(Footer + 216, XmlOffset);
    Be64(Footer + 224, XmlPos);
    Be64(Footer + 492, 1024); // virtual disk is 512 KiB
}
static bool TestFixedAndDynamicDeflate() {
    static const uint8_t Dynamic[] = {
        0x78,0xda,0xed,0xcb,0xb9,0x0d,0xc2,0x40,0x10,0x00,0xc0,0x9c,0x2a,0xb6,0x01,0x28,
        0x80,0xcc,0x09,0x31,0x12,0x15,0x9c,0xcd,0x49,0xc8,0x8f,0xf6,0x64,0x2d,0xfd,0xd3,
        0x05,0xd1,0x4c,0x3e,0xd3,0xf3,0xf1,0x8a,0xa3,0x57,0x7b,0xb7,0x6a,0x31,0xef,0xb9,
        0x6c,0xf7,0x38,0x33,0xeb,0x5a,0x67,0xef,0x91,0xf3,0xda,0x97,0x8a,0xa3,0x8d,0x18,
        0xad,0x3e,0xb1,0x67,0x6e,0xdf,0x71,0xbb,0x4c,0x9a,0xa6,0x69,0x9a,0xa6,0x69,0x9a,
        0xa6,0x69,0x9a,0xa6,0x69,0x9a,0xa6,0x69,0x9a,0xa6,0x69,0x9a,0xa6,0x69,0x9a,
        0xa6,0x69,0x9a,0xa6,0x69,0x9a,0xa6,0x69,0x9a,0xf6,0xcf,0xf6,0x03,0x8b,0x23,0x27,0xca
    };
    static const uint8_t Fixed[] = {
        0x78,0x01,0x73,0xf2,0xf7,0x0f,0x89,0x30,0x33,0xd1,0x75,0x75,0xf3,0xd4,0x0d,0x8a,
        0x70,0x0b,0xd6,0x75,0x0c,0x70,0x0b,0xe6,0x72,0x1a,0x15,0x1d,0x15,0x1d,0x15,0x1d,
        0x15,0x1d,0x15,0x1d,0x15,0x1d,0xd2,0xa2,0x00,0xd7,0xa9,0xce,0x90
    };
    static uint8_t DynamicOut[11000];
    static uint8_t FixedOut[1760];
    uint64_t Actual = 0;
    if (UdifInflateZlib(Dynamic, sizeof(Dynamic), DynamicOut, sizeof(DynamicOut),
                        sizeof(DynamicOut), &Actual) != EFI_SUCCESS || Actual != sizeof(DynamicOut)) return false;
    const char DynamicPattern[] = "APFS metadata block: root-tree object map path lookup.\n";
    for (uint64_t I = 0; I < sizeof(DynamicOut); ++I)
        if (DynamicOut[I] != (uint8_t)DynamicPattern[I % (sizeof(DynamicPattern) - 1)]) return false;
    if (UdifInflateZlib(Fixed, sizeof(Fixed), FixedOut, sizeof(FixedOut),
                        sizeof(FixedOut), &Actual) != EFI_SUCCESS || Actual != sizeof(FixedOut)) return false;
    const char FixedPattern[] = "BOOTX64-EFI-RXFS-APFS\n";
    for (uint64_t I = 0; I < sizeof(FixedOut); ++I)
        if (FixedOut[I] != (uint8_t)FixedPattern[I % (sizeof(FixedPattern) - 1)]) return false;

    uint8_t Bad[sizeof(Dynamic)];
    Copy(Bad, Dynamic, sizeof(Bad));
    Bad[sizeof(Bad) - 1] ^= 1;
    if (UdifInflateZlib(Bad, sizeof(Bad), DynamicOut, sizeof(DynamicOut),
                        sizeof(DynamicOut), &Actual) != EFI_LOAD_ERROR) return false;
    if (UdifInflateZlib(Dynamic, sizeof(Dynamic) - 2, DynamicOut, sizeof(DynamicOut),
                        sizeof(DynamicOut), &Actual) != EFI_LOAD_ERROR) return false;
    if (UdifInflateZlib(Dynamic, sizeof(Dynamic), DynamicOut, sizeof(DynamicOut) - 1,
                        sizeof(DynamicOut), &Actual) != EFI_LOAD_ERROR) return false;
    return true;
}
} // namespace

int main() {
    BuildDmg(1);
    Blob B{gDmg, sizeof(gDmg)};
    APFS_SOURCE Dmg{ReadBlob, &B, sizeof(gDmg)};
    UDIF_CONTEXT Ctx{};
    APFS_SOURCE Apfs{};
    if (UdifOpenApfsPartition(&Dmg, &Ctx, &Apfs) != EFI_SUCCESS) return 1;
    if (Apfs.Size != kApfsBytes || !Ctx.Opened || Ctx.RunCount != 1) return 2;

    uint8_t Header[512];
    if (Apfs.ReadAt(Apfs.Context, 0, sizeof(Header), Header) != EFI_SUCCESS) return 3;
    if (Header[32] != 'N' || Header[33] != 'X' || Header[34] != 'S' || Header[35] != 'B') return 4;

    APFS_CONTAINER_INFO Info{};
    if (ApfsInspectImage(&Apfs, &Info) != EFI_SUCCESS) return 5;
    if (!Info.HasValidGeometry || Info.BlockSize != 4096 || Info.BlockCount != 4) return 6;

    if (Apfs.ReadAt(Apfs.Context, Apfs.Size - 1, 2, Header) != EFI_LOAD_ERROR) return 7;

    BuildDmg(0x80000005U); // zlib-compressed APFS partition using a valid zlib stream
    B.Data = gDmg;
    Dmg.Context = &B;
    Dmg.Size = sizeof(gDmg);
    if (UdifOpenApfsPartition(&Dmg, &Ctx, &Apfs) != EFI_SUCCESS) return 8;
    if (Apfs.ReadAt(Apfs.Context, 0, sizeof(Header), Header) != EFI_SUCCESS) return 9;
    if (Header[32] != 'N' || Header[33] != 'X' || Header[34] != 'S' || Header[35] != 'B') return 10;
    if (ApfsInspectImage(&Apfs, &Info) != EFI_SUCCESS || !Info.HasValidGeometry) return 11;

    BuildDmg(0x80000005U, true); // corrupted Adler-32 must fail closed at read time
    B.Data = gDmg;
    Dmg.Context = &B;
    Dmg.Size = sizeof(gDmg);
    if (UdifOpenApfsPartition(&Dmg, &Ctx, &Apfs) != EFI_SUCCESS) return 12;
    if (Apfs.ReadAt(Apfs.Context, 0, sizeof(Header), Header) != EFI_LOAD_ERROR) return 13;

    if (!TestFixedAndDynamicDeflate()) return 14;

    BuildDmg(1);
    gDmg[kDmgBytes - 512] = 'x'; // invalid trailer
    if (UdifOpenApfsPartition(&Dmg, &Ctx, &Apfs) != EFI_NOT_FOUND) return 15;
    return 0;
}
