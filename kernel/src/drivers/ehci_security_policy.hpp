/* Phase 29: integer-safe bounds and protocol validation used by USB fuzz tests. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::security_policy {
static constexpr uint32_t kMaxConfigBytes=4096u;
static constexpr uint32_t kMaxTransferBytes=16384u;
static constexpr uint32_t kMaxDescriptors=512u;
constexpr bool span_valid(uint32_t total, uint32_t offset, uint32_t length) {
    return offset<=total && length<=total-offset;
}
constexpr bool advance_descriptor(uint32_t total, uint32_t offset, uint8_t descriptor_length,
                                  uint32_t* next) {
    if (!next || descriptor_length<2u || !span_valid(total,offset,descriptor_length)) return false;
    *next=offset+descriptor_length;
    return *next>offset && *next<=total;
}
constexpr bool config_length_valid(uint32_t supplied, uint16_t declared) {
    return supplied>=9u && supplied<=kMaxConfigBytes && declared==supplied;
}
constexpr bool transfer_range_valid(uint32_t address, uint32_t length, uint32_t limit) {
    if (length==0u) return true;
    if (length>kMaxTransferBytes || length>limit || address>0xFFFFFFFFu-length) return false;
    return true;
}
constexpr bool endpoint_valid(uint8_t address, uint8_t attributes, uint16_t packet,
                              uint8_t speed_code) {
    const uint8_t number=static_cast<uint8_t>(address&0x0Fu);
    const uint8_t type=static_cast<uint8_t>(attributes&3u);
    if ((address&0x70u)!=0u || number==0u || packet==0u || type==0u) return false;
    if (speed_code==2u) {
        if (type==2u || type==0u) return packet<=512u;
        return packet<=1024u;
    }
    if (speed_code==1u) return (type==0u || type==3u) && packet<=8u;
    if (speed_code==0u) return type==1u ? packet<=1023u : packet<=64u;
    return false;
}
constexpr bool string_descriptor_valid(const uint8_t* b, uint16_t available) {
    if (!b || available<2u || b[1]!=3u) return false;
    const uint8_t length=b[0];
    return length>=2u && length<=available && (length&1u)==0u;
}
constexpr bool bot_cbw_valid(uint32_t signature, uint32_t tag, uint32_t transfer_length,
                             uint8_t flags, uint8_t lun, uint8_t cdb_length) {
    return signature==0x43425355u && tag!=0u && transfer_length<=kMaxTransferBytes &&
           (flags&0x7Fu)==0u && (transfer_length!=0u || flags==0u) &&
           lun<=15u && cdb_length>=1u && cdb_length<=16u;
}
constexpr bool bot_csw_valid(uint32_t signature, uint32_t expected_tag, uint32_t actual_tag,
                             uint32_t residue, uint32_t requested, uint8_t status) {
    return signature==0x53425355u && expected_tag!=0u && actual_tag==expected_tag &&
           residue<=requested && requested<=kMaxTransferBytes && status<=2u;
}
constexpr bool self_test() {
    uint32_t next=0u;
    if (!span_valid(64u,60u,4u) || span_valid(64u,60u,5u) || span_valid(64u,65u,0u)) return false;
    if (!advance_descriptor(32u,2u,7u,&next) || next!=9u) return false;
    if (advance_descriptor(32u,30u,4u,&next) || advance_descriptor(32u,2u,0u,&next)) return false;
    if (!config_length_valid(9u,9u) || config_length_valid(4097u,4097u) || config_length_valid(16u,15u)) return false;
    if (!transfer_range_valid(0x1000u,4096u,16384u) || transfer_range_valid(0xFFFFFFF0u,64u,16384u) || transfer_range_valid(0u,16385u,20000u)) return false;
    if (!endpoint_valid(0x81u,2u,512u,2u) || endpoint_valid(0x80u,2u,512u,2u) || endpoint_valid(0x81u,2u,513u,2u)) return false;
    const uint8_t good_string[]={4u,3u,'A',0u};
    const uint8_t bad_string[]={3u,3u,'A'};
    if (!string_descriptor_valid(good_string,sizeof(good_string)) || string_descriptor_valid(bad_string,sizeof(bad_string))) return false;
    if (!bot_cbw_valid(0x43425355u,1u,512u,0x80u,0u,10u) || bot_cbw_valid(0u,1u,512u,0x80u,0u,10u) || bot_cbw_valid(0x43425355u,1u,16385u,0u,0u,10u) || bot_cbw_valid(0x43425355u,1u,0u,0x80u,0u,10u)) return false;
    if (!bot_csw_valid(0x53425355u,7u,7u,0u,512u,0u) || bot_csw_valid(0x53425355u,7u,8u,0u,512u,0u) || bot_csw_valid(0x53425355u,7u,7u,513u,512u,0u)) return false;
    return true;
}
} // namespace starlix::drivers::ehci::security_policy
