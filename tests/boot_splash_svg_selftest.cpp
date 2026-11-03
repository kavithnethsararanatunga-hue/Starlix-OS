#include "boot/x64/rxload.efi/src/boot_splash.hpp"
extern "C" int printf(const char*, ...);

static uint32_t pixels[832 * 600 + 1];
static uint32_t mockStallCalls=0u;
static UINTN mockStallTotalUs=0u;
static EFI_STATUS ABI MockStall(UINTN microseconds) {
    ++mockStallCalls;
    mockStallTotalUs += microseconds;
    return EFI_SUCCESS;
}

static uint32_t pixel_at(uint32_t x, uint32_t y) {
    return pixels[y * 832u + x];
}

int main() {
    BOOT_SPLASH_SVG svg{};
    if (!BootSplashParseSvg(kStarlixBootSpinnerSvg, &svg)) return 1;
    if (svg.viewWidth != 64 || svg.viewHeight != 64 ||
        svg.cx != 32 || svg.cy != 32 || svg.radius != 24) return 2;
    if (svg.strokeWidthMilli != 10500 ||
        svg.spinDurationMs != 2200 || svg.pulseDurationMs != 5000) return 3;
    if (svg.dash0 != 2 || svg.dash50 != 70 || svg.dash100 != 2 ||
        svg.offset0 != 0 || svg.offset50 != -40 || svg.offset100 != -153) return 4;
    if (svg.stop0.r != 216 || svg.stop0.g != 219 || svg.stop0.b != 233 ||
        svg.stop1.a != 0) return 5;

    BOOT_SPLASH_STATE s{};
    s.framebuffer = pixels;
    s.width = 800;
    s.height = 600;
    s.stride = 832;
    s.pixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    s.backgroundPixel = BootSplashPackPixel(&s, 0, 0, 0);
    s.exitFadeAlpha = 255u;
    s.spinner = svg;
    s.active = true;

    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = 0; x < s.stride; ++x)
            pixels[y * s.stride + x] = 0xDEADBEEFu;
    pixels[832u * 600u] = 0xA55AA55Au;

    BootSplashFillBackground(&s);
    if (s.backgroundPixel != 0u) return 6;
    const uint32_t logoSize = BootSplashLogoSize(&s);
    if (logoSize != 132u) return 14;
    if (BootSplashSpinnerCenterY(&s) != 444u) return 18;

    // Transparent RGB must not bleed into an edge during downsampling.
    const uint8_t edgeTexture[16] = {
        255, 0, 255, 0,   0, 255, 255, 255,
        0, 255, 0, 0,     0, 255, 255, 255
    };
    const uint32_t edgePixel = BootSplashLogoSamplePixel(&s, edgeTexture, 2, 2, 0, 0, 1, 255);
    const uint8_t edgeR = static_cast<uint8_t>(edgePixel);
    const uint8_t edgeG = static_cast<uint8_t>(edgePixel >> 8);
    const uint8_t edgeB = static_cast<uint8_t>(edgePixel >> 16);
    if (edgeR != 0u || edgeG < 126u || edgeG > 129u || edgeB < 126u || edgeB > 129u) return 19;
    const int32_t logoX = static_cast<int32_t>(s.width / 2u);
    const int32_t logoY = static_cast<int32_t>(s.height / 2u);
    if (pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY)) != s.backgroundPixel) return 15;
    if (logoX != 400 || logoY != 300) return 16;

    // Atomic-frame regression: composing a replacement must leave the live
    // GOP surface untouched until the complete off-screen frame is presented.
    const uint32_t composeSentinel = 0x00123456u;
    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = 0; x < s.stride; ++x)
            pixels[y * s.stride + x] = composeSentinel;
    s.elapsedUs = 1100000ULL;
    BOOT_SPLASH_SPINNER_FRAME composedFrame{};
    if (!BootSplashComposeSpinnerFrame(&s, 255u, gBootSplashSpinnerScratch,
            kBootSplashSpinnerScratchExtent, &composedFrame)) return 35;
    for (uint32_t i = 0; i < 832u * 600u; ++i)
        if (pixels[i] != composeSentinel) return 36;
    uint32_t composedPixels = 0u;
    for (uint32_t y = 0; y < composedFrame.height; ++y)
        for (uint32_t x = 0; x < composedFrame.width; ++x)
            if (gBootSplashSpinnerScratch[y * kBootSplashSpinnerScratchExtent + x] != s.backgroundPixel)
                ++composedPixels;
    if (composedPixels < 100u) return 37;
    BootSplashPresentSpinnerFrame(&s, gBootSplashSpinnerScratch,
        kBootSplashSpinnerScratchExtent, &composedFrame);
    uint32_t presentedPixels = 0u;
    for (uint32_t y = 0; y < composedFrame.height; ++y)
        for (uint32_t x = 0; x < composedFrame.width; ++x)
            if (pixels[(composedFrame.top + y) * s.stride + composedFrame.left + x] != composeSentinel)
                ++presentedPixels;
    if (presentedPixels < composedPixels) return 38;
    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = 0; x < s.stride; ++x) {
            const bool inFrame = x >= composedFrame.left && x < composedFrame.left + composedFrame.width &&
                y >= composedFrame.top && y < composedFrame.top + composedFrame.height;
            if (!inFrame && pixels[y * s.stride + x] != composeSentinel) return 39;
        }
    BootSplashFillBackground(&s);
    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = s.width; x < s.stride; ++x)
            pixels[y * s.stride + x] = 0xDEADBEEFu;

    // Spinner opacity is driven by the same fade alpha as the logo.
    BootSplashFillBackground(&s);
    s.elapsedUs = 1100000ULL;
    BootSplashDrawSpinner(&s, 0u);
    uint32_t invisibleSpinnerPixels = 0u;
    for (uint32_t y = 400; y < 500; ++y)
        for (uint32_t x = 350; x < 450; ++x)
            if (pixel_at(x, y) != s.backgroundPixel) ++invisibleSpinnerPixels;
    if (invisibleSpinnerPixels != 0u) return 20;
    BootSplashFillBackground(&s);
    BootSplashDrawSpinner(&s, 127u);
    uint32_t halfSpinnerPeak = 0u;
    for (uint32_t y = 400; y < 500; ++y)
        for (uint32_t x = 350; x < 450; ++x) {
            const uint32_t p = pixel_at(x, y);
            const uint32_t intensity = (p & 0xFFu) + ((p >> 8u) & 0xFFu) + ((p >> 16u) & 0xFFu);
            if (intensity > halfSpinnerPeak) halfSpinnerPeak = intensity;
        }
    BootSplashFillBackground(&s);
    BootSplashDrawSpinner(&s, 255u);
    uint32_t fullSpinnerPeak = 0u;
    for (uint32_t y = 400; y < 500; ++y)
        for (uint32_t x = 350; x < 450; ++x) {
            const uint32_t p = pixel_at(x, y);
            const uint32_t intensity = (p & 0xFFu) + ((p >> 8u) & 0xFFu) + ((p >> 16u) & 0xFFu);
            if (intensity > fullSpinnerPeak) fullSpinnerPeak = intensity;
        }
    if (halfSpinnerPeak == 0u || fullSpinnerPeak <= halfSpinnerPeak) return 21;

    // The 5-second pulse loop must not jump when elapsed time wraps to zero.
    // 60 Hz timer frames straddle this boundary at 4983 ms -> 5000 ms.
    const BOOT_SPLASH_DASH_FRAME beforePulseWrap = BootSplashEvaluateDashFrame(&svg, 4983u);
    const BOOT_SPLASH_DASH_FRAME atPulseWrap = BootSplashEvaluateDashFrame(&svg, 5000u);
    const uint32_t beforePattern = beforePulseWrap.dash + beforePulseWrap.gap;
    const uint32_t afterPattern = atPulseWrap.dash + atPulseWrap.gap;
    if (!beforePattern || !afterPattern) return 31;
    int32_t beforePhase = beforePulseWrap.offset % static_cast<int32_t>(beforePattern);
    int32_t afterPhase = atPulseWrap.offset % static_cast<int32_t>(afterPattern);
    if (beforePhase < 0) beforePhase += static_cast<int32_t>(beforePattern);
    if (afterPhase < 0) afterPhase += static_cast<int32_t>(afterPattern);
    const uint32_t phaseDistance = beforePhase > afterPhase
        ? static_cast<uint32_t>(beforePhase - afterPhase)
        : static_cast<uint32_t>(afterPhase - beforePhase);
    if (phaseDistance > 2u) return 32;

    // At the final millisecond, the last animated frame is exactly equivalent
    // to the first frame modulo one complete dash-pattern period.
    const BOOT_SPLASH_DASH_FRAME finalPulseFrame = BootSplashEvaluateDashFrame(&svg, 4999u);
    const uint32_t finalPattern = finalPulseFrame.dash + finalPulseFrame.gap;
    if (finalPulseFrame.dash != atPulseWrap.dash ||
        finalPulseFrame.gap != atPulseWrap.gap || !finalPattern) return 33;
    int32_t finalPhase = finalPulseFrame.offset % static_cast<int32_t>(finalPattern);
    if (finalPhase < 0) finalPhase += static_cast<int32_t>(finalPattern);
    if (finalPhase != afterPhase) return 34;

    // The SVG spinner must remain visible at representative animation phases.
    const uint32_t spinnerTimes[] = {0u, 500u, 1100u, 2500u, 4000u};
    for (uint32_t t : spinnerTimes) {
        BootSplashFillBackground(&s);
        s.elapsedUs = static_cast<uint64_t>(t) * 1000ULL;
        BootSplashDrawSpinner(&s, 255u);
        uint32_t spinnerPixels = 0;
        for (uint32_t y = 400; y < 500; ++y)
            for (uint32_t x = 350; x < 450; ++x)
                if (pixel_at(x, y) != s.backgroundPixel) ++spinnerPixels;
        if (spinnerPixels < 100u) return 12;
    }
    // Timer callbacks account real frames. A callback that interrupts a
    // renderer queues its tick; the next safe pump drains it without re-entry.
    s.elapsedUs = 0u;
    s.pendingFrames = 0u;
    __atomic_store_n(&s.renderBusy, 1u, __ATOMIC_RELEASE);
    const uint32_t callbackPixel = pixel_at(360u, 444u);
    BootSplashTimerNotify(nullptr, &s);
    BootSplashTimerNotify(nullptr, &s);
    BootSplashTimerNotify(nullptr, &s);
    if (s.elapsedUs != 0u || s.pendingFrames != 3u ||
        pixel_at(360u, 444u) != callbackPixel) return 29;
    __atomic_store_n(&s.renderBusy, 0u, __ATOMIC_RELEASE);
    if (!BootSplashPump(&s) || s.pendingFrames != 0u ||
        s.elapsedUs != 3ULL * kBootSplashFrameStepUs || s.renderBusy != 0u) return 30;

    BootSplashFillBackground(&s);
    s.logoFadeInitialized = false;
    s.lastLogoFadeAlpha = 0;
    s.elapsedUs = 0u;
    s.pendingFrames = 0u;

    s.elapsedUs = static_cast<uint64_t>(kBootSplashFadeMs) * 500ULL;
    BootSplashRender(&s);
    const uint32_t halfFade = pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY));
    if (halfFade == s.backgroundPixel) return 7;

    s.elapsedUs = static_cast<uint64_t>(kBootSplashFadeMs) * 1000ULL;
    s.exitFadeAlpha = 255u;
    BootSplashRender(&s);
    const uint32_t fullFade = pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY));
    if (fullFade == halfFade || fullFade == s.backgroundPixel) return 8;

    // A transparent corner remains black, and rendering never clears outside the compact centered logo.
    const uint32_t logoLeft = s.width / 2u - logoSize / 2u;
    const uint32_t logoTop = s.height / 2u - logoSize / 2u;
    const uint32_t sentinelX = logoLeft;
    const uint32_t sentinelY = logoTop;
    const uint32_t sentinel = 0x0055AA33u;
    pixels[sentinelY * s.stride + sentinelX] = sentinel;
    BootSplashRender(&s);
    if (pixels[sentinelY * s.stride + sentinelX] != sentinel) return 13;
    const uint32_t stableLogoPixel = pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY));

    for (uint32_t frame = 0; frame < 75; ++frame) {
        s.elapsedUs = static_cast<uint64_t>(frame) * kBootSplashFrameStepUs;
        BootSplashRender(&s);
    }

    if (pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY)) != stableLogoPixel) return 17;

    uint32_t changed = 0;
    for (uint32_t y = 0; y < s.height; ++y) {
        for (uint32_t x = 0; x < s.width; ++x)
            if (pixels[y * s.stride + x] != s.backgroundPixel) ++changed;
        for (uint32_t x = s.width; x < s.stride; ++x)
            if (pixels[y * s.stride + x] != 0xDEADBEEFu) return 9;
    }
    if (pixels[832u * 600u] != 0xA55AA55Au || changed < 1000) return 10;

    // The terminal fade applies one alpha to both the logo and SVG spinner.
    s.exitFadeAlpha = 128u;
    BootSplashRender(&s);
    const uint32_t halfExitFade = pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY));
    if (halfExitFade == s.backgroundPixel || halfExitFade == fullFade) return 22;
    s.exitFadeAlpha = 0u;
    BootSplashRender(&s);
    if (pixel_at(static_cast<uint32_t>(logoX), static_cast<uint32_t>(logoY)) != s.backgroundPixel) return 23;
    s.exitFadeAlpha = 255u;
    s.logoFadeInitialized = false;
    BootSplashRender(&s);

    // Exercise the exact final-memory-map fade routine with a deterministic
    // UEFI Stall mock. It must advance SVG time for 20 frames and end black.
    EFI_BOOT_SERVICES mockBootServices{};
    mockBootServices.Stall = MockStall;
    mockStallCalls = 0u;
    mockStallTotalUs = 0u;
    const uint64_t fadeStartUs = s.elapsedUs;
    if (!BootSplashFadeOutAndStop(&mockBootServices, &s)) return 24;
    if (mockStallCalls != kBootSplashExitFadeFrames ||
        mockStallTotalUs != kBootSplashExitFadeFrames * kBootSplashExitFrameStallUs) return 25;
    if (s.elapsedUs != fadeStartUs + kBootSplashExitFadeFrames * kBootSplashFrameStepUs) return 26;
    if (s.active || s.exitFadeAlpha != 0u) return 27;
    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = 0; x < s.width; ++x)
            if (pixels[y * s.stride + x] != s.backgroundPixel) return 28;


    BOOT_SPLASH_STATE bgr{};
    bgr.pixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    if (BootSplashPackPixel(&bgr, 0x12, 0x34, 0x56) != 0x00123456u) return 11;

    printf("BOOT_SPLASH_SVG_RENDER: PASS (viewBox=%ux%u; spin=%ums; pulse=%ums; logo=%ux%u centered; black-bg; changed=%u; logoFade=PASS; spinnerFade=PASS; exitFade=PASS; cooperativePump=PASS; offscreenCompose=PASS; framePresent=PASS; finalBlack=PASS)\n",
        svg.viewWidth, svg.viewHeight, svg.spinDurationMs, svg.pulseDurationMs, logoSize, logoSize, changed);
    return 0;
}
