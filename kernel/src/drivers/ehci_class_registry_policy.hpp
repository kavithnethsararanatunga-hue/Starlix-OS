/* Phase 28: fail-closed USB interface class dispatch policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::class_registry {
enum class Driver : uint8_t {
    Unsupported=0, Hub=1, MassStorageBOT=2, HID=3, CDC_ACM=4, CDC_Data=5,
    AudioControl=6, AudioStreaming=7, VideoControl=8, VideoStreaming=9,
    Printer=10, EthernetECM=11, EthernetNCM=12, EthernetRNDIS=13,
    Bluetooth=14, SmartCard=15, FirmwareUpdateRestricted=16,
    Diagnostic=17, VendorAllowlisted=18
};
struct Interface { uint8_t class_code, subclass, protocol; };
struct Decision { Driver driver; bool needs_explicit_privilege; bool is_data_interface; };
constexpr Decision classify(Interface i, bool vendor_allowlisted=false) {
    if (i.class_code==0x09u && i.subclass==0u && i.protocol<=1u)
        return {Driver::Hub,false,false};
    if (i.class_code==0x08u && i.subclass==0x06u && i.protocol==0x50u)
        return {Driver::MassStorageBOT,false,false};
    if (i.class_code==0x03u && (i.subclass==0u || i.subclass==1u) &&
        (i.protocol<=2u)) return {Driver::HID,false,false};
    if (i.class_code==0x02u && i.subclass==0x02u)
        return {Driver::CDC_ACM,false,false};
    if (i.class_code==0x0Au) return {Driver::CDC_Data,false,true};
    if (i.class_code==0x01u && i.subclass==0x01u)
        return {Driver::AudioControl,false,false};
    if (i.class_code==0x01u && i.subclass==0x02u)
        return {Driver::AudioStreaming,false,true};
    if (i.class_code==0x0Eu && i.subclass==1u)
        return {Driver::VideoControl,false,false};
    if (i.class_code==0x0Eu && i.subclass==2u)
        return {Driver::VideoStreaming,false,true};
    if (i.class_code==0x07u && i.subclass==1u && i.protocol<=2u)
        return {Driver::Printer,false,false};
    if (i.class_code==0x02u && i.subclass==0x06u)
        return {Driver::EthernetECM,false,false};
    if (i.class_code==0x02u && i.subclass==0x0Du)
        return {Driver::EthernetNCM,false,false};
    if (i.class_code==0xE0u && i.subclass==1u && i.protocol==3u)
        return {Driver::EthernetRNDIS,false,false};
    if (i.class_code==0xE0u && i.subclass==1u && i.protocol==1u)
        return {Driver::Bluetooth,false,false};
    if (i.class_code==0x0Bu && i.subclass==0u)
        return {Driver::SmartCard,false,false};
    if (i.class_code==0xFEu && i.subclass==1u && i.protocol<=2u)
        return {Driver::FirmwareUpdateRestricted,true,false};
    if (i.class_code==0xDCu && i.subclass==1u)
        return {Driver::Diagnostic,false,false};
    if (i.class_code==0xFFu && vendor_allowlisted)
        return {Driver::VendorAllowlisted,true,false};
    return {Driver::Unsupported,false,false};
}
constexpr bool class_claim_allowed(const Decision& d, bool explicit_permission) {
    if (d.driver==Driver::Unsupported || d.driver==Driver::FirmwareUpdateRestricted) return false;
    if (d.driver==Driver::VendorAllowlisted) return explicit_permission;
    return true;
}
constexpr bool self_test() {
    if (classify({0x08u,0x06u,0x50u}).driver!=Driver::MassStorageBOT) return false;
    if (classify({0x03u,1u,1u}).driver!=Driver::HID) return false;
    if (classify({0x02u,2u,1u}).driver!=Driver::CDC_ACM) return false;
    if (classify({0x0Au,0u,0u}).driver!=Driver::CDC_Data) return false;
    if (classify({0x01u,2u,0u}).driver!=Driver::AudioStreaming) return false;
    if (classify({0x0Eu,2u,0u}).driver!=Driver::VideoStreaming) return false;
    if (classify({0x07u,1u,2u}).driver!=Driver::Printer) return false;
    if (classify({0x02u,0x0Du,0u}).driver!=Driver::EthernetNCM) return false;
    if (classify({0xE0u,1u,3u}).driver!=Driver::EthernetRNDIS) return false;
    if (classify({0x0Bu,0u,0u}).driver!=Driver::SmartCard) return false;
    if (classify({0xFFu,0u,0u}).driver!=Driver::Unsupported) return false;
    if (classify({0xFFu,0u,0u},true).driver!=Driver::VendorAllowlisted) return false;
    if (class_claim_allowed(classify({0xFFu,0u,0u},true),false)) return false;
    if (class_claim_allowed(classify({0xFFu,0u,0u},true),true)==false) return false;
    if (class_claim_allowed(classify({0xFEu,1u,1u},false),true)) return false;
    if (class_claim_allowed(classify({0xFEu,1u,1u}),true)) return false;
    return true;
}
} // namespace starlix::drivers::ehci::class_registry
