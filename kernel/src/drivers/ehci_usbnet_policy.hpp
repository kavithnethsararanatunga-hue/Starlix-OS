/* Phase 23: bounded USB networking class, Ethernet frame and ownership policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::usbnet_policy {
constexpr uint32_t kEthernetHeaderBytes=14u;
constexpr uint32_t kMaxEthernetFrame=1518u;
constexpr uint32_t kQueueSlots=4u;
enum class Protocol:uint8_t{Unsupported=0,CDC_ECM=1,CDC_NCM=2,RNDIS=3};
constexpr Protocol classify(uint8_t klass,uint8_t subclass,uint8_t protocol){
    if(klass!=0x02u)return Protocol::Unsupported;
    if(subclass==0x06u)return Protocol::CDC_ECM;
    if(subclass==0x0Du)return Protocol::CDC_NCM;
    if(subclass==0x0Eu&&(protocol==0u||protocol==1u))return Protocol::RNDIS;
    return Protocol::Unsupported;
}
inline bool valid_mac(const uint8_t* mac){if(!mac||(mac[0]&1u)!=0u)return false;uint8_t any=0,all=0xFFu;for(uint8_t i=0;i<6u;++i){any|=mac[i];all&=mac[i];}return any!=0u&&all!=0xFFu;}
constexpr bool valid_frame_length(uint32_t n){return n>=kEthernetHeaderBytes&&n<=kMaxEthernetFrame;}
constexpr bool ncm_block_allowed(uint32_t block_length,uint32_t ndp_offset,uint32_t datagram_offset,uint32_t datagram_length){
    if(block_length<12u||block_length>16384u||ndp_offset<12u||ndp_offset>=block_length||datagram_length<kEthernetHeaderBytes||datagram_length>kMaxEthernetFrame)return false;
    if(datagram_offset<12u||datagram_offset>block_length||datagram_length>block_length-datagram_offset)return false;
    const uint32_t datagram_end=datagram_offset+datagram_length;
    /* NDP has an eight-byte fixed header; reject datagrams overlapping that header. */
    const uint32_t ndp_header_end=ndp_offset+8u;
    if(datagram_offset<ndp_header_end&&ndp_offset<datagram_end)return false;
    return true;
}
struct Slot{uint8_t bytes[kMaxEthernetFrame];uint16_t length;uint32_t generation;};
struct FrameQueue{Slot slots[kQueueSlots];volatile uint32_t head;volatile uint32_t tail;volatile uint32_t dropped;};
inline void queue_init(FrameQueue*q){if(!q)return;__atomic_store_n(&q->head,0u,__ATOMIC_RELAXED);__atomic_store_n(&q->tail,0u,__ATOMIC_RELAXED);__atomic_store_n(&q->dropped,0u,__ATOMIC_RELAXED);for(uint32_t i=0;i<kQueueSlots;++i){q->slots[i].length=0;q->slots[i].generation=0;}}
inline bool enqueue(FrameQueue*q,const uint8_t*frame,uint32_t length,uint32_t generation){if(!q||!frame||!valid_frame_length(length)||generation==0u)return false;const uint32_t h=__atomic_load_n(&q->head,__ATOMIC_RELAXED);const uint32_t next=(h+1u)%kQueueSlots;const uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);if(next==t){__atomic_add_fetch(&q->dropped,1u,__ATOMIC_RELAXED);return false;}for(uint32_t i=0;i<length;++i)q->slots[h].bytes[i]=frame[i];q->slots[h].length=static_cast<uint16_t>(length);q->slots[h].generation=generation;__atomic_store_n(&q->head,next,__ATOMIC_RELEASE);return true;}
inline bool dequeue(FrameQueue*q,uint8_t*out,uint32_t capacity,uint32_t generation,uint16_t*length){if(length)*length=0;if(!q||!out||!length||generation==0u)return false;uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);const uint32_t h=__atomic_load_n(&q->head,__ATOMIC_ACQUIRE);if(t==h)return false;Slot&s=q->slots[t];if(s.generation!=generation){s.length=0;s.generation=0;__atomic_store_n(&q->tail,(t+1u)%kQueueSlots,__ATOMIC_RELEASE);__atomic_add_fetch(&q->dropped,1u,__ATOMIC_RELAXED);return false;}if(s.length>capacity){return false;}for(uint32_t i=0;i<s.length;++i)out[i]=s.bytes[i];*length=s.length;s.length=0;s.generation=0;__atomic_store_n(&q->tail,(t+1u)%kQueueSlots,__ATOMIC_RELEASE);return true;}
inline bool self_test(){if(classify(2u,6u,0u)!=Protocol::CDC_ECM||classify(2u,13u,0u)!=Protocol::CDC_NCM||classify(2u,14u,1u)!=Protocol::RNDIS||classify(3u,1u,1u)!=Protocol::Unsupported)return false;
    const uint8_t mac[6]={0x02u,0x11u,0x22u,0x33u,0x44u,0x55u};const uint8_t multicast[6]={0x01u,0u,0u,0u,0u,1u};const uint8_t zero[6]={0,0,0,0,0,0};if(!valid_mac(mac)||valid_mac(multicast)||valid_mac(zero)||valid_frame_length(13u)||!valid_frame_length(64u)||!valid_frame_length(1518u)||valid_frame_length(1519u))return false;
    if(!ncm_block_allowed(2048u,64u,128u,1500u)||ncm_block_allowed(200u,50u,190u,20u)||ncm_block_allowed(2048u,0u,128u,64u)||ncm_block_allowed(512u,130u,128u,32u))return false;
    static FrameQueue q{};queue_init(&q);uint8_t frame[64]{};frame[0]=0x02u;uint8_t out[64]{};uint16_t len=0;if(!enqueue(&q,frame,sizeof(frame),7u)||!dequeue(&q,out,sizeof(out),7u,&len)||len!=64u||out[0]!=0x02u)return false;if(!enqueue(&q,frame,sizeof(frame),8u)||dequeue(&q,out,sizeof(out),9u,&len))return false;return __atomic_load_n(&q.dropped,__ATOMIC_RELAXED)==1u;}
}
