/* Phase 26: bounded USB Printer Class descriptor, status and job queue policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::printer_policy {
constexpr uint8_t kPrinterClass=0x07u, kPrinterSubclass=0x01u;
constexpr uint32_t kMaxDeviceIdBytes=1024u, kJobQueueBytes=4096u;
enum class State:uint8_t{Offline=0u,Ready=1u,Busy=2u,PaperOut=3u,Fault=4u};
struct Endpoint{uint8_t address;uint8_t attributes;uint16_t max_packet;};
struct EndpointSet{uint8_t bulk_out;uint8_t bulk_in;uint16_t out_packet;uint16_t in_packet;bool has_in;};
constexpr bool valid_interface(uint8_t klass,uint8_t subclass,uint8_t protocol){return klass==kPrinterClass&&subclass==kPrinterSubclass&&protocol<=3u;}
constexpr bool choose_endpoints(const Endpoint* eps,uint32_t count,EndpointSet* out){
 if(!eps||!out||count==0u||count>16u)return false;EndpointSet s{};bool have_out=false,have_in=false;
 for(uint32_t i=0;i<count;++i){const auto&e=eps[i];if((e.address&0x0Fu)==0u||(e.attributes&3u)!=2u||e.max_packet==0u||e.max_packet>512u)continue;
  if((e.address&0x80u)!=0u){if(have_in)return false;have_in=true;s.bulk_in=e.address;s.in_packet=e.max_packet;}else{if(have_out)return false;have_out=true;s.bulk_out=e.address;s.out_packet=e.max_packet;}}
 if(!have_out)return false;s.has_in=have_in;s.bulk_in=have_in?s.bulk_in:0u;s.in_packet=have_in?s.in_packet:0u;*out=s;return true;
}
inline bool validate_device_id(const uint8_t* bytes,uint32_t received,uint16_t* total_length){if(total_length)*total_length=0u;if(!bytes||!total_length||received<2u)return false;const uint32_t declared=(static_cast<uint32_t>(bytes[0])<<8u)|bytes[1];if(declared<2u||declared>kMaxDeviceIdBytes||declared>received)return false;*total_length=static_cast<uint16_t>(declared);return true;}
constexpr State decode_port_status(uint8_t status,bool connected,bool configured){
 if(!connected||!configured)return State::Offline;
 if((status&0x01u)!=0u)return State::PaperOut;
 if((status&0x08u)==0u)return State::Offline;
 if((status&0x10u)==0u)return State::Fault;
 return State::Ready;
}
struct JobQueue{uint8_t bytes[kJobQueueBytes];volatile uint32_t head;volatile uint32_t tail;volatile uint32_t dropped;uint32_t generation;State state;};
inline void queue_init(JobQueue*q,uint32_t generation){if(!q)return;__atomic_store_n(&q->head,0u,__ATOMIC_RELAXED);__atomic_store_n(&q->tail,0u,__ATOMIC_RELAXED);__atomic_store_n(&q->dropped,0u,__ATOMIC_RELAXED);q->generation=generation;q->state=generation?State::Ready:State::Offline;}
inline bool enqueue(JobQueue*q,const uint8_t*data,uint32_t n,uint32_t generation){if(!q||!data||n==0u||n>=kJobQueueBytes||generation==0u||generation!=q->generation||q->state!=State::Ready)return false;const uint32_t h=__atomic_load_n(&q->head,__ATOMIC_RELAXED);const uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);const uint32_t used=(h+kJobQueueBytes-t)%kJobQueueBytes;if(n>kJobQueueBytes-1u-used){__atomic_add_fetch(&q->dropped,1u,__ATOMIC_RELAXED);return false;}for(uint32_t i=0;i<n;++i)q->bytes[(h+i)%kJobQueueBytes]=data[i];__atomic_store_n(&q->head,(h+n)%kJobQueueBytes,__ATOMIC_RELEASE);return true;}
inline uint32_t dequeue(JobQueue*q,uint8_t*out,uint32_t cap,uint32_t generation){if(!q||!out||cap==0u||generation==0u||generation!=q->generation)return 0u;const uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);const uint32_t h=__atomic_load_n(&q->head,__ATOMIC_ACQUIRE);const uint32_t used=(h+kJobQueueBytes-t)%kJobQueueBytes;const uint32_t n=used<cap?used:cap;for(uint32_t i=0;i<n;++i)out[i]=q->bytes[(t+i)%kJobQueueBytes];__atomic_store_n(&q->tail,(t+n)%kJobQueueBytes,__ATOMIC_RELEASE);return n;}
constexpr bool self_test(){if(!valid_interface(7u,1u,2u)||valid_interface(7u,2u,2u)||valid_interface(3u,1u,1u))return false;const Endpoint eps[2]={{0x02u,2u,512u},{0x81u,2u,512u}};EndpointSet set{};if(!choose_endpoints(eps,2u,&set)||!set.has_in||set.bulk_out!=2u)return false;const Endpoint bad[1]={{0x81u,2u,512u}};if(choose_endpoints(bad,1u,&set))return false;const uint8_t id[7]={0u,7u,'M','F','G','X',';'};uint16_t length=0u;if(!validate_device_id(id,sizeof(id),&length)||length!=7u)return false;const uint8_t truncated[2]={0u,8u};if(validate_device_id(truncated,sizeof(truncated),&length))return false;const uint8_t too_big[2]={4u,1u};if(validate_device_id(too_big,sizeof(too_big),&length))return false;if(decode_port_status(0u,true,true)!=State::Offline||decode_port_status(0x18u,true,true)!=State::Ready||decode_port_status(1u,true,true)!=State::PaperOut||decode_port_status(0x08u,true,true)!=State::Fault||decode_port_status(0x18u,false,true)!=State::Offline)return false;static JobQueue q{};queue_init(&q,9u);uint8_t data[5]={1u,2u,3u,4u,5u},out[5]{};if(!enqueue(&q,data,5u,9u)||dequeue(&q,out,5u,9u)!=5u||out[4]!=5u||dequeue(&q,out,5u,9u)!=0u)return false;if(enqueue(&q,data,5u,8u))return false;return true;}
} // namespace starlix::drivers::ehci::printer_policy
