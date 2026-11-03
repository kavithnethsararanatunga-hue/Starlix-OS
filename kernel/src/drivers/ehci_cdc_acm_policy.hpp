/* Phase 22: CDC ACM descriptor, serial-format and ring-buffer policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::cdc_acm_policy {
constexpr uint32_t kRingCapacity=1024u;
enum class InterfaceRole:uint8_t{Unsupported=0,ControlACM=1,Data=2};
struct Endpoint { uint8_t address; uint8_t attributes; uint16_t max_packet; };
struct EndpointPair { uint8_t bulk_in; uint8_t bulk_out; uint16_t max_packet_in; uint16_t max_packet_out; };
struct LineCoding { uint32_t baud; uint8_t stop_bits; uint8_t parity; uint8_t data_bits; };
constexpr InterfaceRole classify_interface(uint8_t klass,uint8_t subclass,uint8_t protocol) {
    if(klass==0x02u && subclass==0x02u && (protocol==0u||protocol==1u))return InterfaceRole::ControlACM;
    if(klass==0x0Au)return InterfaceRole::Data;
    return InterfaceRole::Unsupported;
}
constexpr bool valid_line_coding(const LineCoding& c) {
    return c.baud>=50u&&c.baud<=3000000u&&c.stop_bits<=2u&&c.parity<=4u&&
        ((c.data_bits>=5u&&c.data_bits<=8u)||c.data_bits==16u);
}
constexpr bool valid_control_line_state(uint16_t state){return (state&~0x0003u)==0u;}
constexpr bool choose_bulk_pair(const Endpoint* eps,uint32_t count,EndpointPair* out) {
    if(!eps||!out||count==0u||count>16u)return false; EndpointPair p{}; bool in=false,out_seen=false;
    for(uint32_t i=0;i<count;++i){const Endpoint&e=eps[i];if((e.attributes&3u)!=2u||e.max_packet==0u||e.max_packet>512u||(e.address&0x0Fu)==0u)continue;
        if((e.address&0x80u)!=0u){if(in)return false;in=true;p.bulk_in=e.address;p.max_packet_in=e.max_packet;}
        else {if(out_seen)return false;out_seen=true;p.bulk_out=e.address;p.max_packet_out=e.max_packet;}}
    if(!in||!out_seen||p.bulk_in==p.bulk_out)return false;*out=p;return true;
}
struct RingBuffer { uint8_t bytes[kRingCapacity]; volatile uint32_t head; volatile uint32_t tail; volatile uint32_t dropped; };
inline void ring_init(RingBuffer* r){if(!r)return;__atomic_store_n(&r->head,0u,__ATOMIC_RELAXED);__atomic_store_n(&r->tail,0u,__ATOMIC_RELAXED);__atomic_store_n(&r->dropped,0u,__ATOMIC_RELAXED);}
inline bool ring_push(RingBuffer* r,uint8_t v){if(!r)return false;const uint32_t h=__atomic_load_n(&r->head,__ATOMIC_RELAXED);const uint32_t next=(h+1u)%kRingCapacity;const uint32_t t=__atomic_load_n(&r->tail,__ATOMIC_ACQUIRE);if(next==t){__atomic_add_fetch(&r->dropped,1u,__ATOMIC_RELAXED);return false;}r->bytes[h]=v;__atomic_store_n(&r->head,next,__ATOMIC_RELEASE);return true;}
inline bool ring_pop(RingBuffer* r,uint8_t* v){if(!r||!v)return false;const uint32_t t=__atomic_load_n(&r->tail,__ATOMIC_RELAXED);const uint32_t h=__atomic_load_n(&r->head,__ATOMIC_ACQUIRE);if(t==h)return false;*v=r->bytes[t];__atomic_store_n(&r->tail,(t+1u)%kRingCapacity,__ATOMIC_RELEASE);return true;}
inline bool self_test(){if(classify_interface(2u,2u,1u)!=InterfaceRole::ControlACM||classify_interface(0x0Au,0u,0u)!=InterfaceRole::Data||classify_interface(3u,1u,1u)!=InterfaceRole::Unsupported)return false;
    if(!valid_line_coding({115200u,0u,0u,8u})||valid_line_coding({0u,0u,0u,8u})||valid_line_coding({115200u,3u,0u,8u})||valid_line_coding({115200u,0u,5u,8u})||!valid_line_coding({9600u,2u,2u,7u}))return false;
    if(!valid_control_line_state(3u)||valid_control_line_state(4u))return false;
    const Endpoint ep[2]={{0x81u,2u,64u},{0x02u,2u,64u}};EndpointPair pair{};if(!choose_bulk_pair(ep,2u,&pair)||pair.bulk_in!=0x81u||pair.bulk_out!=2u)return false;
    const Endpoint bad[2]={{0x81u,2u,64u},{0x81u,2u,64u}};if(choose_bulk_pair(bad,2u,&pair))return false;
    RingBuffer ring{};ring_init(&ring);uint8_t value=0;if(ring_pop(&ring,&value))return false;for(uint32_t i=0;i<kRingCapacity-1u;++i)if(!ring_push(&ring,static_cast<uint8_t>(i)))return false;if(ring_push(&ring,0u)||__atomic_load_n(&ring.dropped,__ATOMIC_RELAXED)!=1u)return false;for(uint32_t i=0;i<kRingCapacity-1u;++i)if(!ring_pop(&ring,&value)||value!=static_cast<uint8_t>(i))return false;if(ring_pop(&ring,&value))return false;return true;}
}
