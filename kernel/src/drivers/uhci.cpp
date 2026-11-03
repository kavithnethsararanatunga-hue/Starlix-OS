/* USB 1.1 UHCI host controller for VMware's full-/low-speed root ports. */
#include "uhci.hpp"
#include "uhci_policy.hpp"
#include "pci.hpp"
#include "serial.hpp"
#include "../cpu/idt.hpp"
#include "../cpu/io.hpp"
#include "../mem/phys.hpp"

namespace starlix::drivers::uhci {
using starlix::drivers::ehci::EHCIPeriodicPollResult;
using starlix::drivers::ehci::USB_SETUP_PACKET;
namespace {
constexpr uint32_t PAGE = 4096u;
constexpr uint32_t FRAMES = 1024u;
constexpr uint32_t ENDPOINTS = 8u;
constexpr uint32_t QH_PER_PAGE = PAGE / 16u;
constexpr uint32_t QH_PAGES = FRAMES / QH_PER_PAGE;
constexpr uint32_t MAX_CONTROL = 4096u;
constexpr uint32_t TERM = 1u;
constexpr uint16_t CMD_RS = 1u;
constexpr uint16_t CMD_HCRESET = 2u;
constexpr uint16_t CMD_CONFIGURE = 1u << 6u;
constexpr uint16_t STS_HALTED = 1u << 5u;
constexpr uint16_t PORT_CSC = 1u << 1u;
constexpr uint16_t PORT_PE = 1u << 2u;
constexpr uint16_t PORT_PEC = 1u << 3u;
constexpr uint16_t PORT_RESET = 1u << 9u;
constexpr uint32_t TD_ACTIVE = 1u << 23u;
constexpr uint32_t TD_NAK = 1u << 19u;
constexpr uint32_t TD_IOC = 1u << 24u;
constexpr uint32_t TD_LOW_SPEED = 1u << 26u;
constexpr uint32_t TD_ERR3 = 3u << 27u;
constexpr uint32_t TD_SPD = 1u << 29u;
constexpr uint32_t TD_ERROR_MASK = (1u << 17u) | (1u << 18u) | (1u << 20u) | (1u << 21u) | (1u << 22u);
constexpr uint8_t PID_SETUP = 0x2Du;
constexpr uint8_t PID_IN = 0x69u;
constexpr uint8_t PID_OUT = 0xE1u;

struct alignas(16) QH { volatile uint32_t horizontal; volatile uint32_t element; uint32_t reserved[2]; };
struct alignas(16) TD { volatile uint32_t link; volatile uint32_t status; volatile uint32_t token; volatile uint32_t buffer; };
static_assert(sizeof(QH) == 16u && sizeof(TD) == 16u);

struct PeriodicEndpoint {
    bool active;
    bool low_speed;
    uint8_t address;
    uint8_t endpoint;
    uint8_t interval;
    uint8_t toggle;
    uint16_t packet;
    uint32_t id;
    TD* td;
    uint32_t td_phys;
    uint8_t* buffer;
    uint32_t buffer_phys;
    uint32_t node_count;
    starlix::mem::DmaRegion td_page;
    starlix::mem::DmaRegion buffer_page;
    starlix::mem::DmaRegion qh_pages[QH_PAGES];
};
struct Device { bool used; bool low; uint8_t ep0; };

static bool active = false;
static bool quarantined = false;
static uint16_t io = 0;
static uint8_t port_count = 0;
static uint8_t controller_bus = 0, controller_slot = 0, controller_function = 0;
static uint32_t lock = 0;
static uint32_t control_lock = 0;
static uint32_t endpoint_id_next = 1;
static uint32_t endpoint_active = 0;
static uint32_t report_completion_count = 0u;
static uint32_t transfer_error_count = 0u;
static uint32_t no_data_count = 0u;
static uint32_t nak_count = 0u;
static bool enumeration_active = false;
static bool enumeration_low = false;
static uint8_t port_address[2]{};
static Device devices[128]{};
static PeriodicEndpoint periodic[ENDPOINTS]{};
static starlix::mem::DmaRegion frame_region{}, schedule_region{}, td_region{}, buffer_region{};
static uint32_t* frames = nullptr;
static QH* control_qh = nullptr;
static TD* control_td = nullptr;
static uint8_t* control_buffer = nullptr;
static uint32_t control_qh_phys = 0, control_td_phys = 0, control_buffer_phys = 0;

static inline uint16_t in16(uint16_t off) { return starlix::cpu::inw(static_cast<uint16_t>(io + off)); }
static inline void out16(uint16_t off, uint16_t v) { starlix::cpu::outw(static_cast<uint16_t>(io + off), v); }
static inline void out32(uint16_t off, uint32_t v) { starlix::cpu::outl(static_cast<uint16_t>(io + off), v); }
static inline void barrier() { asm volatile("mfence" ::: "memory"); }
static bool acquire() { uint32_t e=0; return __atomic_compare_exchange_n(&lock,&e,1u,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void release() { __atomic_store_n(&lock,0u,__ATOMIC_RELEASE); }
struct ControlTransferGuard {
    bool acquired;
    ControlTransferGuard() : acquired(false) {
        uint32_t expected=0u;
        acquired=__atomic_compare_exchange_n(&control_lock,&expected,1u,false,
                                             __ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    ~ControlTransferGuard() { if(acquired)__atomic_store_n(&control_lock,0u,__ATOMIC_RELEASE); }
};

static void hex16(uint16_t v) {
    const char* h="0123456789ABCDEF"; char s[5]{};
    for (int i=3;i>=0;--i) { s[i]=h[v&15u]; v=static_cast<uint16_t>(v>>4u); }
    starlix::drivers::serial::print(s);
}
static bool dma_page(starlix::mem::DmaRegion& r) {
    return starlix::mem::dma32_alloc_page(r) && r.valid && r.cpu_address && r.length >= PAGE &&
        r.device_address <= 0xFFFFF000ull && (r.device_address & (PAGE-1u)) == 0;
}
static bool resolve(const void* p,uint64_t n,uint64_t a,uint32_t& out) {
    if (!starlix::mem::dma32_resolve(p,n,a,out)) return false;
    return static_cast<uint64_t>(out)+n-1u <= 0xFFFFFFFFull;
}
static bool delay_ms(uint32_t ms) {
    if (!starlix::cpu::timer_is_ready()) return false;
    const uint64_t end=starlix::cpu::timer_now_ms()+ms;
    while (starlix::cpu::timer_now_ms()<end) asm volatile("pause" ::: "memory");
    return true;
}
static bool wait_halted(bool halted,uint32_t ms) {
    if (!starlix::cpu::timer_is_ready()) return false;
    const uint64_t end=starlix::cpu::timer_now_ms()+ms;
    do { if (((in16(0x02u)&STS_HALTED)!=0u)==halted) return true; asm volatile("pause" ::: "memory"); }
    while (starlix::cpu::timer_now_ms()<=end);
    return ((in16(0x02u)&STS_HALTED)!=0u)==halted;
}
static bool run(bool enabled) {
    uint16_t cmd=in16(0u);
    if (enabled) cmd=static_cast<uint16_t>(cmd|CMD_RS|CMD_CONFIGURE);
    else cmd=static_cast<uint16_t>(cmd&~CMD_RS);
    out16(0u,cmd);
    return wait_halted(!enabled,100u);
}
static uint32_t token(uint8_t pid,uint8_t addr,uint8_t ep,bool toggle,uint16_t n) {
    const uint32_t length=n ? static_cast<uint32_t>(n-1u) : 0x7FFu;
    return pid | ((static_cast<uint32_t>(addr)&0x7Fu)<<8u) |
        ((static_cast<uint32_t>(ep)&0x0Fu)<<15u) | (toggle ? (1u<<19u):0u) |
        ((length&0x7FFu)<<21u);
}
static bool set_td(QH* qh,uint32_t qh_phys,TD* td,uint32_t td_phys,
                   uint8_t addr,uint8_t ep,bool low,uint8_t pid,bool toggle,
                   void* data,uint16_t length,uint32_t extra,uint16_t* actual) {
    if (actual) *actual=0;
    if (!active || quarantined || !qh || !td || length>1023u || !acquire()) return false;
    if (!run(false)) { quarantined=true; release(); return false; }
    uint32_t data_phys=0;
    if (length && (!data || !resolve(data,length,1u,data_phys) ||
        ((data_phys&0xFFFu)+length)>PAGE)) {
        if(!run(true))quarantined=true; release(); return false;
    }
    td->link=TERM;
    td->status=TD_ACTIVE|TD_ERR3|(low?TD_LOW_SPEED:0u)|TD_IOC|extra;
    td->token=token(pid,addr,ep,toggle,length);
    td->buffer=length?data_phys:0xFFFFFFFFu;
    if (length && !starlix::mem::dma32_sync_for_device(data,length)) {
        if(!run(true))quarantined=true; release(); return false;
    }
    if (!starlix::mem::dma32_sync_for_device(td,sizeof(*td))) {
        if(!run(true))quarantined=true; release(); return false;
    }
    qh->element=policy::link_td(td_phys);
    if (!starlix::mem::dma32_sync_for_device(qh,sizeof(*qh))) {
        qh->element=TERM; if(!run(true))quarantined=true; release(); return false;
    }
    barrier();
    if (!run(true)) { quarantined=true; release(); return false; }
    const uint64_t deadline=starlix::cpu::timer_now_ms()+500u;
    bool done=false,ok=false;
    while (starlix::cpu::timer_now_ms()<=deadline) {
        if (!starlix::mem::dma32_sync_for_cpu(td,sizeof(*td))) break;
        barrier();
        const uint32_t status=td->status;
        if ((status&TD_ACTIVE)==0u) { done=true; ok=(status&TD_ERROR_MASK)==0u; break; }
        asm volatile("pause" ::: "memory");
    }
    if (!run(false)) { quarantined=true; release(); return false; }
    /* Controller halt is the DMA lifetime fence before descriptor reuse. */
    qh->element=TERM;
    if (!starlix::mem::dma32_sync_for_device(qh,sizeof(*qh)) ||
        !starlix::mem::dma32_sync_for_cpu(td,sizeof(*td))) { quarantined=true; release(); return false; }
    barrier();
    const uint32_t status=td->status;
    if (ok && length && (pid==PID_IN) && !starlix::mem::dma32_sync_for_cpu(data,length)) ok=false;
    if (actual && done) *actual=policy::actual_length(status);
    const bool result=done && ok;
    if(!done){
        ++transfer_error_count;
        starlix::drivers::serial::print("[UHCI][TRANSFER] Transaction deadline expired; TD safely halted before reuse.\n");
    } else if(!ok){
        ++transfer_error_count;
        if(transfer_error_count==1u||(transfer_error_count&0x1Fu)==0u){
            starlix::drivers::serial::print("[UHCI][TRANSFER] TD reported USB error status=0x");
            hex16(static_cast<uint16_t>(status>>16u)); starlix::drivers::serial::print(".\n");
        }
    }
    if (!run(true)) { quarantined=true; release(); return false; }
    release();
    (void)qh_phys;
    return result;
}

static bool init_storage() {
    if (!dma_page(frame_region)||!dma_page(schedule_region)||!dma_page(td_region)||!dma_page(buffer_region)) return false;
    frames=static_cast<uint32_t*>(frame_region.cpu_address);
    control_qh=static_cast<QH*>(schedule_region.cpu_address);
    control_td=static_cast<TD*>(td_region.cpu_address);
    control_buffer=static_cast<uint8_t*>(buffer_region.cpu_address);
    uint32_t frame_phys=0;
    if (!resolve(frames,PAGE,PAGE,frame_phys)) return false;
    frame_region.device_address=frame_phys;
    return true;
}

} // namespace

bool uhci_init() {
    if (active) return true;
    if (!policy::self_test()) { starlix::drivers::serial::print("[UHCI][POLICY] validation self-test FAILED.\n"); return false; }
    starlix::drivers::serial::print("[UHCI][POLICY] BAR, DMA, port, TD and packet checks passed.\n");
    uint8_t bus=0,slot=0,function=0; uint32_t bar=0;
    if (!starlix::drivers::pci::pci_find_uhci(bus,slot,function,bar)) {
        starlix::drivers::serial::print("[UHCI] No supported USB 1.1 controller found.\n"); return false;
    }
    const uint16_t base=static_cast<uint16_t>(bar&0xFFFCu);
    if (!policy::io_bar_valid(bar|1u,&io) || io!=base) return false;
    const uint32_t vd=starlix::drivers::pci::pci_read(bus,slot,function,0x00u);
    const uint32_t command=starlix::drivers::pci::pci_read(bus,slot,function,0x04u);
    starlix::drivers::pci::pci_write(bus,slot,function,0x04u,(command&0xFFFFu)|0x0005u);
    const uint32_t verify=starlix::drivers::pci::pci_read(bus,slot,function,0x04u);
    if ((verify&5u)!=5u) { starlix::drivers::serial::print("[UHCI] PCI I/O and bus-master enable failed.\n"); return false; }
    controller_bus=bus; controller_slot=slot; controller_function=function;
    starlix::drivers::serial::print("[UHCI] PCI "); hex16(static_cast<uint16_t>(vd)); starlix::drivers::serial::print(":");
    hex16(static_cast<uint16_t>(vd>>16u)); starlix::drivers::serial::print(" BAR4 I/O=0x"); hex16(io); starlix::drivers::serial::print(".\n");
    /* Disable interrupts, reset, and require reset self-clear within 100 ms. */
    out16(0x04u,0u); out16(0x02u,0xFFFFu);
    out16(0x00u,CMD_HCRESET);
    if (!starlix::cpu::timer_is_ready()) return false;
    const uint64_t reset_deadline=starlix::cpu::timer_now_ms()+100u;
    while ((in16(0x00u)&CMD_HCRESET)!=0u && starlix::cpu::timer_now_ms()<=reset_deadline) asm volatile("pause" ::: "memory");
    if (in16(0x00u)&CMD_HCRESET) { starlix::drivers::serial::print("[UHCI] Host-controller reset timed out.\n"); return false; }
    if (!init_storage()) { starlix::drivers::serial::print("[UHCI][DMA] DMA32 frame/schedule allocation failed.\n"); return false; }
    if (!resolve(control_qh,sizeof(*control_qh),16u,control_qh_phys)||
        !resolve(control_td,PAGE,16u,control_td_phys)||
        !resolve(control_buffer,PAGE,PAGE,control_buffer_phys)) return false;
    control_qh->horizontal=TERM; control_qh->element=TERM;
    for (uint32_t f=0;f<FRAMES;++f) frames[f]=policy::link_qh(control_qh_phys);
    if (!starlix::mem::dma32_sync_for_device(frames,PAGE)||
        !starlix::mem::dma32_sync_for_device(control_qh,sizeof(*control_qh))) return false;
    out16(0x0Cu,0u); out16(0x06u,0u); out32(0x08u,static_cast<uint32_t>(frame_region.device_address));
    out16(0x0Au,0x40u); out16(0x04u,0xFFFFu);
    uint16_t cmd=CMD_CONFIGURE|(1u<<7u); out16(0x00u,cmd);
    if (!run(true)) { starlix::drivers::serial::print("[UHCI] Controller failed to enter RUNNING state.\n"); return false; }
    active=true;
    /* UHCI 1.1 defines exactly two root-port status registers. */
    port_count=2u;
    starlix::drivers::serial::print("[UHCI] Host controller RUNNING; frame-list DMA and bounded port polling active. Root ports: ");
    hex16(port_count); starlix::drivers::serial::print(".\n");
    uhci_scan_ports();
    return true;
}

bool uhci_is_active() { return active && !quarantined; }
void uhci_begin_enumeration(bool low) { enumeration_active=true; enumeration_low=low; }
void uhci_end_enumeration() { enumeration_active=false; }
bool uhci_owns_device(uint8_t address) { return address!=0u && address<128u && devices[address].used; }
void uhci_set_device(uint8_t address,bool low,uint8_t ep0) {
    if (address<128u && address!=0u) devices[address]=Device{true,low,ep0};
    if (address==0u) devices[0]=Device{true,low,ep0};
}
void uhci_set_port_device_address(uint8_t port,uint8_t address) { if(port<2u)port_address[port]=address; }

bool uhci_control_transfer(uint8_t address,USB_SETUP_PACKET* setup,void* data,uint16_t length) {
    if (!active||quarantined||!setup||address>127u||setup->wLength!=length||length>MAX_CONTROL||
        (length&&!data)) return false;
    ControlTransferGuard guard; if(!guard.acquired)return false;
    const Device dev=devices[address];
    const bool low=(address==0u&&enumeration_active)?enumeration_low:(dev.used?dev.low:enumeration_low);
    const uint8_t packet=dev.ep0?dev.ep0:8u;
    if (!policy::packet_valid(low,0u,packet)) return false;
    uint8_t setup_bytes[8]={setup->bmRequestType,setup->bRequest,
        static_cast<uint8_t>(setup->wValue),static_cast<uint8_t>(setup->wValue>>8u),
        static_cast<uint8_t>(setup->wIndex),static_cast<uint8_t>(setup->wIndex>>8u),
        static_cast<uint8_t>(setup->wLength),static_cast<uint8_t>(setup->wLength>>8u)};
    for (uint32_t i=0;i<8u;++i) control_buffer[i]=setup_bytes[i];
    uint16_t actual=0;
    if (!set_td(control_qh,control_qh_phys,control_td,control_td_phys,address,0u,low,PID_SETUP,false,
                control_buffer,8u,0u,&actual)||actual!=8u) return false;
    const bool input=(setup->bmRequestType&0x80u)!=0u;
    if (length&&!input) for (uint32_t i=0;i<length;++i) control_buffer[i]=static_cast<uint8_t*>(data)[i];
    uint16_t total=0;
    for (uint32_t off=0;off<length;) {
        const uint16_t n=static_cast<uint16_t>((length-off)>packet?packet:(length-off));
        if (input) for (uint16_t j=0;j<n;++j) control_buffer[off+j]=0;
        if (!set_td(control_qh,control_qh_phys,control_td,control_td_phys,address,0u,low,
                input?PID_IN:PID_OUT,((off/packet)&1u)==0u,control_buffer+off,n,
                input?TD_SPD:0u,&actual)) return false;
        if (actual>n) return false;
        if (input) for (uint16_t j=0;j<actual;++j) static_cast<uint8_t*>(data)[off+j]=control_buffer[off+j];
        total=static_cast<uint16_t>(total+actual);
        off=static_cast<uint32_t>(off+actual);
        if (actual<n) break; /* UHCI SPD ends the data stage; issue status next. */
    }
    const uint8_t status_pid=(length==0u||!input)?PID_IN:PID_OUT;
    if (!set_td(control_qh,control_qh_phys,control_td,control_td_phys,address,0u,low,status_pid,true,
                nullptr,0u,0u,&actual)||actual!=0u) return false;
    if (input&&length) (void)total;
    return true;
}

static QH* qh_node(PeriodicEndpoint& ep,uint32_t frame,uint32_t& phys) {
    const uint32_t page=frame/QH_PER_PAGE,idx=frame%QH_PER_PAGE;
    if (page>=QH_PAGES||!ep.qh_pages[page].valid) return nullptr;
    QH* q=&static_cast<QH*>(ep.qh_pages[page].cpu_address)[idx];
    return resolve(q,sizeof(*q),16u,phys)?q:nullptr;
}
static bool rebuild_schedule() {
    if (!active||quarantined||!run(false)) { quarantined=true; return false; }
    control_qh->horizontal=TERM; control_qh->element=TERM;
    uint32_t counts[ENDPOINTS]{};
    for (uint32_t f=0;f<FRAMES;++f) {
        uint32_t first=0; QH* previous=nullptr;
        for (uint32_t e=0;e<ENDPOINTS;++e) {
            PeriodicEndpoint& ep=periodic[e];
            if (!ep.active||f%ep.interval) continue;
            uint32_t qphys=0; QH* q=qh_node(ep,f,qphys); if (!q) return false;
            q->horizontal=TERM; q->element=policy::link_td(ep.td_phys); q->reserved[0]=q->reserved[1]=0;
            if (previous) { uint32_t pphys=0; if (!resolve(previous,sizeof(*previous),16u,pphys)) return false; previous->horizontal=policy::link_qh(qphys); }
            else first=qphys;
            previous=q; ++counts[e];
        }
        if (previous) { uint32_t dummy=0; if (!resolve(previous,sizeof(*previous),16u,dummy)) return false; previous->horizontal=policy::link_qh(control_qh_phys); }
        frames[f]=policy::link_qh(first?first:control_qh_phys);
    }
    for (uint32_t e=0;e<ENDPOINTS;++e) {
        PeriodicEndpoint& ep=periodic[e]; ep.node_count=counts[e];
        for (uint32_t p=0;p<QH_PAGES;++p) if (ep.qh_pages[p].valid &&
            !starlix::mem::dma32_sync_for_device(ep.qh_pages[p].cpu_address,PAGE)) return false;
    }
    if (!starlix::mem::dma32_sync_for_device(frames,PAGE)||
        !starlix::mem::dma32_sync_for_device(control_qh,sizeof(*control_qh))) return false;
    barrier(); out32(0x08u,static_cast<uint32_t>(frame_region.device_address));
    return run(true);
}

bool uhci_periodic_interrupt_open(uint8_t address,uint8_t endpoint,uint16_t packet,uint8_t interval,uint32_t* id) {
    if (!id) return false; *id=0;
    if (!active||quarantined||address==0u||address>127u||!devices[address].used||
        !(endpoint&0x80u)||(endpoint&0x70u)||(endpoint&0x0Fu)==0u||
        !policy::packet_valid(devices[address].low,endpoint&0x0Fu,packet)||!interval||interval>255u||!acquire()) return false;
    PeriodicEndpoint* ep=nullptr; uint32_t index=0;
    for(uint32_t i=0;i<ENDPOINTS;++i) if(!periodic[i].active){ep=&periodic[i];index=i;break;}
    if(!ep){release();return false;}
    bool ok=dma_page(ep->td_page); ok=dma_page(ep->buffer_page)&&ok;
    for(uint32_t p=0;p<QH_PAGES;++p) ok=dma_page(ep->qh_pages[p])&&ok;
    if(!ok){release();return false;}
    ep->td=static_cast<TD*>(ep->td_page.cpu_address); ep->buffer=static_cast<uint8_t*>(ep->buffer_page.cpu_address);
    if(!resolve(ep->td,sizeof(TD),16u,ep->td_phys)||!resolve(ep->buffer,packet,PAGE,ep->buffer_phys)){release();return false;}
    ep->active=true; ep->low_speed=devices[address].low; ep->address=address; ep->endpoint=endpoint;
    ep->packet=packet; ep->interval=policy::frame_interval(interval); ep->toggle=0u;
    ep->id=0x80000000u|endpoint_id_next++; if(endpoint_id_next&0x80000000u)endpoint_id_next=1u;
    ep->td->link=TERM; ep->td->status=TD_ACTIVE|TD_ERR3|TD_IOC|(ep->low_speed?TD_LOW_SPEED:0u);
    ep->td->token=token(PID_IN,address,endpoint&0x0Fu,false,packet); ep->td->buffer=ep->buffer_phys;
    if(!starlix::mem::dma32_sync_for_device(ep->buffer,PAGE)||!starlix::mem::dma32_sync_for_device(ep->td,sizeof(TD))){ep->active=false;release();return false;}
    ++endpoint_active; const bool ok_schedule=rebuild_schedule();
    if(!ok_schedule){ep->active=false;--endpoint_active;(void)rebuild_schedule();release();return false;}
    *id=ep->id; release();
    starlix::drivers::serial::print("[UHCI][PERIODIC] Interrupt-IN QH scheduled on full-/low-speed frame list.\n");
    (void)index; return true;
}

static PeriodicEndpoint* find_ep(uint32_t id){for(uint32_t i=0;i<ENDPOINTS;++i)if(periodic[i].active&&periodic[i].id==id)return &periodic[i];return nullptr;}
static bool rearm_consumed_qhs(PeriodicEndpoint& ep) {
    for(uint32_t f=0;f<FRAMES;++f) {
        if(f%ep.interval)continue;
        uint32_t qphys=0; QH* q=qh_node(ep,f,qphys); if(!q)return false;
        if(!starlix::mem::dma32_sync_for_cpu(q,sizeof(*q)))return false;
        if(policy::link_terminated(q->element)) {
            q->element=policy::link_td(ep.td_phys);
            if(!starlix::mem::dma32_sync_for_device(q,sizeof(*q)))return false;
        }
    }
    return true;
}
EHCIPeriodicPollResult uhci_periodic_interrupt_poll(uint32_t id,void* out,uint16_t cap,uint16_t* actual) {
    if(actual)*actual=0; if(!(id&0x80000000u)||!active||!acquire())return EHCIPeriodicPollResult::InvalidEndpoint;
    PeriodicEndpoint* ep=find_ep(id); if(!ep){release();return EHCIPeriodicPollResult::InvalidEndpoint;}
    if(!starlix::mem::dma32_sync_for_cpu(ep->td,sizeof(TD))){release();return EHCIPeriodicPollResult::TransferError;}
    barrier(); const uint32_t st=ep->td->status;
    if(st&TD_ACTIVE){
        if(no_data_count!=0xFFFFFFFFu)++no_data_count;
        if(st&TD_NAK){
            if(nak_count!=0xFFFFFFFFu)++nak_count;
            if(nak_count==1u||(nak_count&0xFFFu)==0u)
                starlix::drivers::serial::print("[UHCI][TRANSFER] Interrupt-IN NAK/no-data; TD remains active for controller retry.\n");
            } else if(no_data_count==1u) {
            starlix::drivers::serial::print("[UHCI][TRANSFER] Interrupt-IN pending; no completed TD yet.\n");
        }
        release();return EHCIPeriodicPollResult::NoData;
    }
    if(st&TD_ERROR_MASK){
        if(transfer_error_count!=0xFFFFFFFFu)++transfer_error_count;
        if(transfer_error_count==1u||(transfer_error_count&0x3Fu)==0u){
            starlix::drivers::serial::print("[UHCI][TRANSFER] Interrupt-IN TD error status=0x");
            hex16(static_cast<uint16_t>(st>>16u));starlix::drivers::serial::print("; rearming descriptor.\n");
        }
        if(!rearm_consumed_qhs(*ep)){quarantined=true;release();return EHCIPeriodicPollResult::TransferError;}
        ep->td->status=TD_ACTIVE|TD_ERR3|TD_IOC|(ep->low_speed?TD_LOW_SPEED:0u);
        ep->td->token=token(PID_IN,ep->address,ep->endpoint&15u,ep->toggle!=0u,ep->packet);
        ep->td->buffer=ep->buffer_phys; (void)starlix::mem::dma32_sync_for_device(ep->td,sizeof(TD)); release();
        return EHCIPeriodicPollResult::TransferError;
    }
    const uint16_t n=policy::actual_length(st);
    if(n>cap||n>ep->packet||(n&&!out)){release();return EHCIPeriodicPollResult::BufferTooSmall;}
    if(!starlix::mem::dma32_sync_for_cpu(ep->buffer,PAGE)){release();return EHCIPeriodicPollResult::TransferError;}
    for(uint16_t i=0;i<n;++i)static_cast<uint8_t*>(out)[i]=ep->buffer[i];
    if(!rearm_consumed_qhs(*ep)){quarantined=true;release();return EHCIPeriodicPollResult::TransferError;}
    ep->toggle^=1u;
    ep->td->status=TD_ACTIVE|TD_ERR3|TD_IOC|(ep->low_speed?TD_LOW_SPEED:0u);
    ep->td->token=token(PID_IN,ep->address,ep->endpoint&15u,ep->toggle!=0u,ep->packet);
    ep->td->buffer=ep->buffer_phys;
    if(!starlix::mem::dma32_sync_for_device(ep->td,sizeof(TD))){release();return EHCIPeriodicPollResult::TransferError;}
    barrier(); if(actual)*actual=n;
    if(n!=0u){
        ++report_completion_count;
        if(report_completion_count==1u || (report_completion_count&0x7Fu)==0u){
            starlix::drivers::serial::print("[UHCI][TRANSFER] Interrupt-IN TD complete bytes=0x");
            hex16(n); starlix::drivers::serial::print(" td-status=0x");
            hex16(static_cast<uint16_t>(st>>16u));
            starlix::drivers::serial::print("; report content omitted.\n");
        }
    }
    release(); return EHCIPeriodicPollResult::Completed;
}
bool uhci_periodic_interrupt_close(uint32_t id){
    if(!active||!acquire())return false; PeriodicEndpoint* ep=find_ep(id); if(!ep){release();return false;}
    ep->active=false; if(endpoint_active)--endpoint_active; const bool ok=rebuild_schedule(); if(!ok)ep->active=true;
    release(); return ok;
}
uint32_t uhci_periodic_interrupt_active_count(){return __atomic_load_n(&endpoint_active,__ATOMIC_ACQUIRE);}
bool uhci_forget_device(uint8_t addr){
    if(addr==0u||addr>127u)return false; uint32_t ids[ENDPOINTS]{};uint32_t n=0;
    for(uint32_t i=0;i<ENDPOINTS;++i)if(periodic[i].active&&periodic[i].address==addr)ids[n++]=periodic[i].id;
    bool ok=true;for(uint32_t i=0;i<n;++i)if(!uhci_periodic_interrupt_close(ids[i]))ok=false;
    if(ok)devices[addr]=Device{};return ok;
}

static uint16_t read_port(uint8_t p){return in16(static_cast<uint16_t>(0x10u+2u*p));}
static void write_port(uint8_t p,uint16_t control,uint16_t clear){
    const uint16_t value=static_cast<uint16_t>((control&~(PORT_CSC|PORT_PEC))|(clear&(PORT_CSC|PORT_PEC)));
    out16(static_cast<uint16_t>(0x10u+2u*p),value);
}
static bool reset_port(uint8_t p,bool& low){
    uint16_t st=read_port(p); if(!policy::port_connected(st))return false;
    low=policy::port_low_speed(st); write_port(p,(st|PORT_RESET)&~(PORT_PE|PORT_CSC|PORT_PEC),st&(PORT_CSC|PORT_PEC));
    if(!delay_ms(50u)){write_port(p,st&~(PORT_RESET|PORT_PE),0u);return false;}
    write_port(p,st&~(PORT_RESET|PORT_PE),0u);
    if(!delay_ms(10u))return false;
    st=read_port(p); if(!policy::port_connected(st))return false;
    /* UHCI enable is software-controlled after reset; preserve the sampled speed bit. */
    write_port(p,static_cast<uint16_t>((st|PORT_PE)&~(PORT_CSC|PORT_PEC)),st&(PORT_CSC|PORT_PEC));
    if(!delay_ms(10u))return false;
    st=read_port(p); return policy::port_connected(st)&&(st&PORT_PE)!=0u;
}
void uhci_scan_ports(){
    if(!active)return;
    for(uint8_t p=0;p<port_count;++p){uint16_t st=read_port(p);
        if(policy::port_change(st))write_port(p,st,st&(PORT_CSC|PORT_PEC));
        st=read_port(p); const bool connected=policy::port_connected(st);
        if(!connected){if(port_address[p]){const uint8_t a=port_address[p];port_address[p]=0; (void)starlix::drivers::ehci::ehci_companion_disconnect(p,a);}
            continue;}
        if(port_address[p])continue;
        if(!delay_ms(100u))continue;
        if(!policy::port_connected(read_port(p)))continue;
        bool low=false; if(!reset_port(p,low)){starlix::drivers::serial::print("[UHCI][PORT] Port reset failed or disconnected.\n");continue;}
        starlix::drivers::serial::print(low?"[UHCI][PORT] Stable low-speed connection after reset.\n":"[UHCI][PORT] Stable full-speed connection after reset.\n");
        starlix::drivers::ehci::ehci_companion_enumerate(p,low);
    }
}
bool uhci_port_change_pending(){if(!active)return false;for(uint8_t p=0;p<port_count;++p)if(policy::port_change(read_port(p)))return true;return false;}
bool uhci_service_port_changes(){if(!active||!uhci_port_change_pending())return false;starlix::drivers::serial::print("[UHCI][HOTPLUG] Polled root-port change; scanning companion ports.\n");uhci_scan_ports();return true;}

} // namespace starlix::drivers::uhci
