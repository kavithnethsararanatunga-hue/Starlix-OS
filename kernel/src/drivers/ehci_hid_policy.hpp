/* Phase 21: bounded USB HID descriptor/report and boot input policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::hid_policy {
enum class Binding : uint8_t { None=0, BootKeyboard=1, BootMouse=2, GenericHid=3 };
enum class KeyStatus : uint8_t { Valid=0, Rollover=1, Malformed=2 };
struct HidDescriptorSummary { uint16_t report_length; uint8_t descriptor_count; };
struct ReportSummary {
    uint32_t input_bits;
    uint32_t item_count;
    uint8_t report_id_count;
    bool has_keyboard;
    bool has_mouse;
    bool uses_report_ids;
};
struct BootKeyboardReport { uint8_t modifiers; uint8_t keys[6]; };
struct BootMouseReport { uint8_t buttons; int16_t dx; int16_t dy; int8_t wheel; bool has_wheel; };
static constexpr uint8_t GENERIC_MOUSE_REPORT_CAPACITY = 8u;
struct GenericMouseReportLayout {
    uint8_t report_id;
    uint8_t x_bits, y_bits, wheel_bits;
    uint8_t button_count;
    uint16_t report_bits;
    uint16_t x_offset, y_offset, wheel_offset;
    uint16_t button_offsets[8];
    bool has_x, has_y, has_wheel, x_signed, y_signed, wheel_signed;
};
struct GenericMouseDescriptor {
    uint8_t report_count;
    GenericMouseReportLayout reports[GENERIC_MOUSE_REPORT_CAPACITY];
};
constexpr Binding classify_interface(uint8_t klass, uint8_t subclass, uint8_t protocol) {
    if (klass != 0x03u) return Binding::None;
    if (subclass == 1u && protocol == 1u) return Binding::BootKeyboard;
    if (subclass == 1u && protocol == 2u) return Binding::BootMouse;
    return Binding::GenericHid;
}
constexpr bool validate_hid_descriptor(const uint8_t* b, uint32_t length,
                                       HidDescriptorSummary* out) {
    if (!b || !out || length < 9u || b[0] < 9u || b[0] > length || b[1] != 0x21u)
        return false;
    const uint8_t count = b[5];
    if (count == 0u || count > 8u || 6u + static_cast<uint32_t>(count) * 3u > b[0]) return false;
    uint16_t report_length = 0;
    bool has_report = false;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* d = b + 6u + static_cast<uint32_t>(i) * 3u;
        if (d[0] == 0x22u) { report_length = static_cast<uint16_t>(d[1] | (static_cast<uint16_t>(d[2]) << 8u)); has_report = true; }
    }
    if (!has_report || report_length == 0u || report_length > 4096u) return false;
    out->report_length = report_length; out->descriptor_count = count;
    return true;
}
inline uint32_t item_unsigned(const uint8_t* p, uint8_t n) {
    uint32_t v=0; for (uint8_t i=0;i<n;++i) v |= static_cast<uint32_t>(p[i]) << (8u*i); return v;
}
inline bool parse_report_descriptor(const uint8_t* b, uint32_t length, ReportSummary* out) {
    if (!b || !out || length == 0u || length > 4096u) return false;
    ReportSummary s{}; uint32_t report_size=0, report_count=0, usage_page=0, usage=0;
    uint32_t app_usage_page=0, app_usage=0; uint32_t offset=0; uint8_t depth=0;
    while (offset < length) {
        const uint8_t prefix=b[offset++];
        if (prefix==0xFEu) return false; // bounded parser deliberately rejects long items
        const uint8_t sc=prefix & 3u; const uint8_t n=(sc==3u)?4u:sc;
        const uint8_t type=(prefix>>2u)&3u; const uint8_t tag=(prefix>>4u)&0x0Fu;
        if (offset+n > length) return false;
        const uint32_t v=item_unsigned(b+offset,n); offset+=n;
        if (++s.item_count > 512u) return false;
        if (type==1u) {
            if (tag==0u) usage_page=v;
            else if (tag==7u) { if (v>32u) return false; report_size=v; }
            else if (tag==8u) { if (v==0u || v>255u) return false; s.uses_report_ids=true; if(s.report_id_count<255u)++s.report_id_count; }
            else if (tag==9u) { if(v>1024u)return false; report_count=v; }
            else if (tag==10u || tag==11u) return false; // unsupported global PUSH/POP
        } else if (type==2u) {
            if (tag==0u) usage=v;
        } else if (type==0u) {
            if (tag==10u) { if(depth>=8u)return false; ++depth; if(depth==1u){app_usage_page=usage_page;app_usage=usage;} }
            else if (tag==12u) { if(depth==0u)return false; --depth; if(depth==0u){app_usage_page=0;app_usage=0;} }
            else if (tag==8u) { // Input
                const uint64_t bits=static_cast<uint64_t>(report_size)*report_count;
                if (bits > 32768u || s.input_bits + bits > 65536u) return false;
                s.input_bits += static_cast<uint32_t>(bits);
                if(depth!=0u && app_usage_page==1u && app_usage==6u) s.has_keyboard=true;
                if(depth!=0u && app_usage_page==1u && app_usage==2u) s.has_mouse=true;
                usage=0;
            } else if(tag==9u || tag==11u) usage=0; // Output/Feature consume local usage
            else usage=0;
        }
    }
    if(depth!=0u || s.input_bits==0u) return false;
    *out=s; return true;
}
inline KeyStatus decode_boot_keyboard(const uint8_t* b, uint32_t length, BootKeyboardReport* out) {
    if(!b||!out||length!=8u)return KeyStatus::Malformed;
    out->modifiers=b[0];
    for(uint8_t i=0;i<6u;++i){out->keys[i]=b[2u+i];if(out->keys[i]>=1u&&out->keys[i]<=3u)return KeyStatus::Rollover;}
    return KeyStatus::Valid;
}
inline bool decode_boot_mouse(const uint8_t* b,uint32_t length,BootMouseReport* out) {
    if(!b||!out||(length!=3u&&length!=4u))return false;
    out->buttons=b[0]&0x07u;out->dx=static_cast<int8_t>(b[1]);out->dy=static_cast<int8_t>(b[2]);
    out->has_wheel=length==4u;out->wheel=out->has_wheel?static_cast<int8_t>(b[3]):0;return true;
}
inline int32_t item_signed(uint32_t value, uint8_t bytes) {
    if (bytes == 0u || bytes >= 4u) return static_cast<int32_t>(value);
    const uint32_t sign = 1u << (bytes * 8u - 1u);
    return (value & sign) ? static_cast<int32_t>(value | (~0u << (bytes * 8u)))
                          : static_cast<int32_t>(value);
}
inline bool parse_generic_mouse_descriptor(const uint8_t* b, uint32_t length,
                                           GenericMouseDescriptor* out) {
    if (!b || !out || length == 0u || length > 4096u) return false;
    struct Globals { uint32_t page; int32_t logical_min; uint8_t size; uint16_t count; uint8_t id; };
    Globals g{0u,0,0u,0u,0u}, stack[8]{};
    uint8_t stack_depth=0u, collection_depth=0u, mouse_depth=0u;
    uint32_t local_usage[32]{}; uint8_t local_count=0u;
    uint32_t usage_min=0u,usage_max=0u; bool has_min=false,has_max=false;
    uint16_t offsets[256]{};
    bool report_id_seen[256]{};
    GenericMouseDescriptor result{};
    uint32_t off=0u,items=0u;
    while(off<length) {
        const uint8_t prefix=b[off++];
        if(prefix==0xFEu || ++items>512u) return false;
        const uint8_t size_code=prefix&3u;
        const uint8_t n=size_code==3u?4u:size_code;
        const uint8_t type=(prefix>>2u)&3u,tag=(prefix>>4u)&0x0Fu;
        if(off+n>length)return false;
        const uint32_t raw=item_unsigned(b+off,n); off+=n;
        if(type==1u) {
            if(tag==0u) { if(raw>0xFFFFu)return false;g.page=raw; }
            else if(tag==1u)g.logical_min=item_signed(raw,n);
            else if(tag==7u) {if(raw==0u||raw>16u)return false;g.size=static_cast<uint8_t>(raw);}
            else if(tag==8u) {
                if(raw==0u||raw>255u)return false;
                report_id_seen[raw]=true;
                g.id=static_cast<uint8_t>(raw);
            } else if(tag==9u) {if(raw==0u||raw>64u)return false;g.count=static_cast<uint16_t>(raw);}
            else if(tag==10u) {if(stack_depth>=8u)return false;stack[stack_depth++]=g;}
            else if(tag==11u) {if(stack_depth==0u)return false;g=stack[--stack_depth];}
        } else if(type==2u) {
            if(tag==0u) {if(local_count>=32u)return false;local_usage[local_count++]=raw;}
            else if(tag==1u) {usage_min=raw;has_min=true;}
            else if(tag==2u) {usage_max=raw;has_max=true;}
        } else if(type==0u) {
            if(tag==10u) {
                if(collection_depth>=8u)return false;
                const bool app_mouse=raw==1u&&g.page==1u&&local_count!=0u&&local_usage[0]==2u;
                if(collection_depth==0u)mouse_depth=app_mouse?1u:0u;
                else if(mouse_depth!=0u)++mouse_depth;
                ++collection_depth;
                local_count=0u;has_min=false;has_max=false;
            } else if(tag==12u) {
                if(collection_depth==0u)return false;
                --collection_depth;
                if(mouse_depth!=0u) {--mouse_depth;}
                local_count=0u;has_min=false;has_max=false;
            } else if(tag==8u) {
                if(g.size==0u||g.count==0u)return false;
                const uint64_t bit_count=static_cast<uint64_t>(g.size)*g.count;
                if(bit_count>512u||static_cast<uint32_t>(offsets[g.id])+bit_count>512u)return false;
                const uint16_t base=offsets[g.id];
                const bool data=(raw&1u)==0u,variable=(raw&2u)!=0u,relative=(raw&4u)!=0u;
                if(mouse_depth!=0u&&data&&variable) {
                    uint8_t ri=0u;
                    while(ri<result.report_count&&result.reports[ri].report_id!=g.id)++ri;
                    if(ri==result.report_count) {
                        if(result.report_count>=GENERIC_MOUSE_REPORT_CAPACITY)return false;
                        result.reports[ri]=GenericMouseReportLayout{};
                        result.reports[ri].report_id=g.id;++result.report_count;
                    }
                    GenericMouseReportLayout& r=result.reports[ri];
                    for(uint16_t f=0;f<g.count;++f) {
                        uint32_t usage=0u;
                        if(f<local_count)usage=local_usage[f];
                        else if(has_min&&has_max&&usage_min<=usage_max&&usage_min+f<=usage_max)usage=usage_min+f;
                        else if(local_count)usage=local_usage[local_count-1u];
                        const uint32_t bit=static_cast<uint32_t>(base)+static_cast<uint32_t>(f)*g.size;
                        if(g.page==9u&&usage>=1u&&usage<=8u) {
                            if(g.size!=1u)return false;
                            const uint8_t button=static_cast<uint8_t>(usage-1u);
                            if(button>=r.button_count)r.button_count=static_cast<uint8_t>(button+1u);
                            r.button_offsets[button]=static_cast<uint16_t>(bit);
                        } else if(g.page==1u&&relative&&usage==0x30u) {
                            if(r.has_x)return false;r.has_x=true;r.x_offset=static_cast<uint16_t>(bit);r.x_bits=g.size;r.x_signed=g.logical_min<0;
                        } else if(g.page==1u&&relative&&usage==0x31u) {
                            if(r.has_y)return false;r.has_y=true;r.y_offset=static_cast<uint16_t>(bit);r.y_bits=g.size;r.y_signed=g.logical_min<0;
                        } else if(g.page==1u&&relative&&usage==0x38u) {
                            if(r.has_wheel)return false;r.has_wheel=true;r.wheel_offset=static_cast<uint16_t>(bit);r.wheel_bits=g.size;r.wheel_signed=g.logical_min<0;
                        } else if(g.page==1u&&(usage==0x30u||usage==0x31u||usage==0x38u)&&!relative) {
                            return false;
                        }
                    }
                }
                offsets[g.id]=static_cast<uint16_t>(base+bit_count);
                local_count=0u;has_min=false;has_max=false;
            } else if(tag==9u||tag==11u) {local_count=0u;has_min=false;has_max=false;}
            else {local_count=0u;has_min=false;has_max=false;}
        }
    }
    if(collection_depth!=0u||stack_depth!=0u||result.report_count==0u)return false;
    if(g.id!=0u&&!report_id_seen[g.id])return false;
    for(uint8_t i=0;i<result.report_count;++i) {
        GenericMouseReportLayout& r=result.reports[i];
        r.report_bits=offsets[r.report_id];
        if(!r.has_x||!r.has_y||r.report_bits==0u)return false;
    }
    *out=result;return true;
}
inline bool decode_generic_mouse_report(const GenericMouseReportLayout& layout,
                                        const uint8_t* report,uint32_t length,
                                        BootMouseReport* out) {
    if(!report||!out||layout.report_bits==0u||layout.report_bits>512u||
       layout.x_bits==0u||layout.y_bits==0u||layout.x_bits>16u||layout.y_bits>16u||
       (layout.has_wheel&&(layout.wheel_bits==0u||layout.wheel_bits>16u)))return false;
    const uint32_t prefix=layout.report_id?1u:0u;
    const uint32_t expected=prefix+(layout.report_bits+7u)/8u;
    if(length!=expected||(prefix&&report[0]!=layout.report_id))return false;
    const uint8_t* data=report+prefix;
    auto field=[&](uint16_t offset,uint8_t bits,bool is_signed,int16_t* value)->bool {
        if(!value||bits==0u||bits>16u||static_cast<uint32_t>(offset)+bits>layout.report_bits)return false;
        uint32_t raw=0u;
        for(uint8_t i=0;i<bits;++i)raw|=static_cast<uint32_t>((data[(offset+i)/8u]>>((offset+i)%8u))&1u)<<i;
        if(is_signed&&(raw&(1u<<(bits-1u))))raw|=(~0u<<bits);
        else if(!is_signed&&bits==16u&&raw>0x7FFFu)return false;
        *value=static_cast<int16_t>(static_cast<int32_t>(raw));return true;
    };
    BootMouseReport decoded{}; int16_t value=0;
    if(!field(layout.x_offset,layout.x_bits,layout.x_signed,&decoded.dx)||!field(layout.y_offset,layout.y_bits,layout.y_signed,&decoded.dy))return false;
    for(uint8_t button=0;button<layout.button_count&&button<8u;++button) {
        if(static_cast<uint32_t>(layout.button_offsets[button])+1u>layout.report_bits)return false;
        if((data[layout.button_offsets[button]/8u]>>(layout.button_offsets[button]%8u))&1u)
            decoded.buttons|=static_cast<uint8_t>(1u<<button);
    }
    if(layout.has_wheel) {
        if(!field(layout.wheel_offset,layout.wheel_bits,layout.wheel_signed,&value))return false;
        if(value>127)value=127;if(value< -128)value=-128;
        decoded.wheel=static_cast<int8_t>(value);decoded.has_wheel=true;
    }
    *out=decoded;return true;
}
constexpr bool self_test() {
    if(classify_interface(3u,1u,1u)!=Binding::BootKeyboard || classify_interface(3u,1u,2u)!=Binding::BootMouse || classify_interface(3u,0u,0u)!=Binding::GenericHid || classify_interface(2u,2u,1u)!=Binding::None)return false;
    const uint8_t hid[9]={9u,0x21u,0x11u,0x01u,0u,1u,0x22u,63u,0u}; HidDescriptorSummary hs{};
    if(!validate_hid_descriptor(hid,sizeof(hid),&hs)||hs.report_length!=63u)return false;
    uint8_t bad_hid[9]={9u,0x21u,0x11u,1u,0u,1u,0x23u,63u,0u}; if(validate_hid_descriptor(bad_hid,sizeof(bad_hid),&hs))return false;
    const uint8_t report[]={0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0};
    ReportSummary rs{}; if(!parse_report_descriptor(report,sizeof(report),&rs)||!rs.has_keyboard)return false;
    const uint8_t rollover[8]={2u,0u,1u,0u,0u,0u,0u,0u}; BootKeyboardReport kr{}; if(decode_boot_keyboard(rollover,sizeof(rollover),&kr)!=KeyStatus::Rollover)return false;
    const uint8_t key[8]={2u,0u,4u,5u,0u,0u,0u,0u}; if(decode_boot_keyboard(key,sizeof(key),&kr)!=KeyStatus::Valid||kr.modifiers!=2u||kr.keys[0]!=4u)return false;
    const uint8_t mouse[4]={3u,0xFFu,2u,1u}; BootMouseReport mr{}; if(!decode_boot_mouse(mouse,sizeof(mouse),&mr)||mr.dx!=-1||mr.dy!=2||mr.wheel!=1)return false;
    if(decode_boot_mouse(mouse,2u,&mr))return false;
    const uint8_t generic_mouse[]={0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,
        0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,
        0x75,0x05,0x95,0x01,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,
        0x75,0x08,0x95,0x02,0x81,0x06,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x01,0x81,0x06,0xC0,0xC0};
    GenericMouseDescriptor gm{};if(!parse_generic_mouse_descriptor(generic_mouse,sizeof(generic_mouse),&gm)||gm.report_count!=1u)return false;
    const uint8_t generic_report[]={0x05,0xFD,0x02,0x01};
    if(!decode_generic_mouse_report(gm.reports[0],generic_report,sizeof(generic_report),&mr)||
       mr.buttons!=5u||mr.dx!=-3||mr.dy!=2||mr.wheel!=1)return false;
    if(decode_generic_mouse_report(gm.reports[0],generic_report,3u,&mr))return false;
    GenericMouseReportLayout out_of_bounds=gm.reports[0];out_of_bounds.x_offset=510u;
    if(decode_generic_mouse_report(out_of_bounds,generic_report,sizeof(generic_report),&mr))return false;
    const uint8_t report_id_mouse[]={0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x01,
        0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x03,0x81,0x02,0x75,0x05,0x95,0x01,0x81,0x01,
        0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,0x81,0x06,
        0x09,0x38,0x75,0x08,0x95,0x01,0x81,0x06,0x85,0x02,
        0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,0x81,0x06,0xC0,0xC0};
    GenericMouseDescriptor gm_id{};
    if(!parse_generic_mouse_descriptor(report_id_mouse,sizeof(report_id_mouse),&gm_id)||gm_id.report_count!=2u)return false;
    const uint8_t report_with_id[]={1u,5u,0xFDu,2u,1u};
    if(!decode_generic_mouse_report(gm_id.reports[0],report_with_id,sizeof(report_with_id),&mr)||
       mr.buttons!=5u||mr.dx!=-3||mr.dy!=2||mr.wheel!=1)return false;
    const uint8_t wrong_report_id[]={2u,5u,0xFDu,2u,1u};
    if(decode_generic_mouse_report(gm_id.reports[0],wrong_report_id,sizeof(wrong_report_id),&mr))return false;
    const uint8_t report_id_2[]={2u,0xFDu,2u};
    if(!decode_generic_mouse_report(gm_id.reports[1],report_id_2,sizeof(report_id_2),&mr)||
       mr.dx!=-3||mr.dy!=2||mr.has_wheel||mr.buttons!=0u)return false;
    const uint8_t bad_generic[]={0x05,0x01,0x09};if(parse_generic_mouse_descriptor(bad_generic,sizeof(bad_generic),&gm))return false;
    const uint8_t trunc[]={0x75,0x08,0x95}; if(parse_report_descriptor(trunc,sizeof(trunc),&rs))return false;
    return true;
}
}
