/* Phase 25: bounded UVC streaming descriptor, payload and frame policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::video_policy {
constexpr uint8_t kVideoClass=0x0Eu, kStreamingSubclass=0x02u;
constexpr uint8_t kIsochronous=1u, kBulk=2u;
constexpr uint32_t kMaxWidth=1920u, kMaxHeight=1080u, kMaxFrameBytes=16u*1024u*1024u;
constexpr uint32_t kFrameQueueSlots=4u;
enum class Format:uint8_t { YUY2=1u, MJPEG=2u, Gray8=3u };
struct StreamConfig { uint8_t interface_class; uint8_t interface_subclass; uint8_t transfer_type; uint16_t max_packet; uint16_t width; uint16_t height; Format format; uint32_t max_frame_bytes; uint32_t buffer_bytes; uint8_t queue_slots; uint32_t generation; };
constexpr uint32_t required_frame_bytes(uint32_t width,uint32_t height,Format f) {
    if(width==0u||height==0u||width>kMaxWidth||height>kMaxHeight)return 0u;
    const uint32_t bpp=(f==Format::YUY2)?2u:((f==Format::Gray8)?1u:0u);
    if(bpp==0u)return 0u;
    const uint64_t v=static_cast<uint64_t>(width)*height*bpp;
    return v>kMaxFrameBytes?0u:static_cast<uint32_t>(v);
}
constexpr bool valid_stream(const StreamConfig& c) {
    if(c.interface_class!=kVideoClass||c.interface_subclass!=kStreamingSubclass||c.width==0u||c.height==0u||c.width>kMaxWidth||c.height>kMaxHeight||c.max_frame_bytes==0u||c.max_frame_bytes>kMaxFrameBytes||c.buffer_bytes<c.max_frame_bytes||c.queue_slots==0u||c.queue_slots>kFrameQueueSlots||c.generation==0u)return false;
    if(c.transfer_type!=kIsochronous&&c.transfer_type!=kBulk)return false;
    if(c.max_packet==0u||(c.transfer_type==kIsochronous&&c.max_packet>1024u)||(c.transfer_type==kBulk&&c.max_packet>512u))return false;
    if(c.format==Format::YUY2||c.format==Format::Gray8){const uint32_t expected=required_frame_bytes(c.width,c.height,c.format);return expected!=0u&&c.max_frame_bytes>=expected;}
    if(c.format==Format::MJPEG)return c.max_frame_bytes<=c.buffer_bytes;
    return false;
}
struct PayloadHeader { uint8_t length; uint8_t flags; bool fid; bool eof; bool error; uint8_t pts_bytes; uint8_t scr_bytes; };
inline bool parse_payload_header(const uint8_t* b,uint32_t length,PayloadHeader* out) {
    if(!b||!out||length<2u||b[0]<2u||b[0]>length||(b[1]&0x80u)==0u)return false;
    const uint8_t pts=(b[1]&0x04u)?4u:0u; const uint8_t scr=(b[1]&0x08u)?6u:0u;
    if(static_cast<uint32_t>(b[0])<2u+pts+scr)return false;
    if((b[1]&0x40u)!=0u)return false;
    out->length=b[0];out->flags=b[1];out->fid=(b[1]&1u)!=0u;out->eof=(b[1]&2u)!=0u;out->error=false;out->pts_bytes=pts;out->scr_bytes=scr;return true;
}
struct FrameState { uint32_t generation; uint32_t bytes; bool active; bool complete; bool dropped; bool fid; uint32_t completed_frames; uint32_t dropped_frames; };
inline void frame_reset(FrameState* s,uint32_t generation){if(!s)return;s->generation=generation;s->bytes=0u;s->active=false;s->complete=false;s->dropped=false;s->fid=false;s->completed_frames=0u;s->dropped_frames=0u;}
inline bool accept_payload(FrameState* s,bool fid,bool eof,bool error,uint32_t payload_bytes,uint32_t max_frame_bytes,uint32_t generation){
    if(!s||generation==0u||s->generation!=generation||max_frame_bytes==0u||max_frame_bytes>kMaxFrameBytes||payload_bytes==0u)return false;
    if(error){s->dropped=true;s->active=false;++s->dropped_frames;return false;}
    if(!s->active||s->fid!=fid){if(s->active&&!s->complete){s->dropped=true;++s->dropped_frames;}s->active=true;s->complete=false;s->dropped=false;s->fid=fid;s->bytes=0u;}
    if(payload_bytes>max_frame_bytes-s->bytes){s->dropped=true;s->active=false;++s->dropped_frames;return false;}
    s->bytes+=payload_bytes;if(eof){s->complete=true;s->active=false;++s->completed_frames;}return true;
}
constexpr bool self_test(){
    const StreamConfig yuy2{0x0Eu,2u,1u,1024u,640u,480u,Format::YUY2,640u*480u*2u,640u*480u*2u,2u,1u};
    const StreamConfig mjpeg{0x0Eu,2u,2u,512u,1280u,720u,Format::MJPEG,2u*1024u*1024u,2u*1024u*1024u,4u,4u};
    if(!valid_stream(yuy2)||!valid_stream(mjpeg)||required_frame_bytes(1920u,1080u,Format::YUY2)!=4147200u)return false;
    StreamConfig bad=yuy2;bad.max_packet=1025u;if(valid_stream(bad))return false;bad=yuy2;bad.width=3000u;if(valid_stream(bad))return false;bad=yuy2;bad.buffer_bytes=10u;if(valid_stream(bad))return false;
    const uint8_t h1[2]={2u,0x83u};PayloadHeader h{};if(!parse_payload_header(h1,2u,&h)||!h.fid||!h.eof)return false;
    const uint8_t hpts[6]={6u,0x84u,1u,2u,3u,4u};if(!parse_payload_header(hpts,sizeof(hpts),&h)||h.pts_bytes!=4u||h.scr_bytes!=0u)return false;
    const uint8_t trunc[2]={2u,0x8Cu};if(parse_payload_header(trunc,sizeof(trunc),&h))return false;const uint8_t err[2]={2u,0xC0u};if(parse_payload_header(err,sizeof(err),&h))return false;
    FrameState st{};frame_reset(&st,5u);if(!accept_payload(&st,false,false,false,100u,200u,5u)||!accept_payload(&st,false,true,false,100u,200u,5u)||st.completed_frames!=1u||st.bytes!=200u)return false;
    if(accept_payload(&st,false,true,false,1u,200u,4u))return false;frame_reset(&st,7u);if(accept_payload(&st,true,false,false,101u,100u,7u)||st.dropped_frames!=1u)return false;
    return !accept_payload(&st,false,true,true,10u,100u,7u);
}
} // namespace starlix::drivers::ehci::video_policy
