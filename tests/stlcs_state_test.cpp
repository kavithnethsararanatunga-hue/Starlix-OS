#include "../user_components/stlcs/stlcs_abi.hpp"

using namespace starlix::stlcs;

namespace {
struct MockHost {
    InputEvent events[160]{};
    stlcs_u32 event_count{};
    stlcs_u32 event_index{};
    stlcs_u32 present_count{};
    stlcs_u32 auth_count{};
    stlcs_u32 desktop_count{};
    stlcs_u32 yield_count{};
    stlcs_u64 monotonic_ms{1000u};
    stlcs_u32 last_presented_transition_alpha{};
    AuthResult next_auth{AuthResult::Rejected};
    bool allow_desktop{true};
    bool allow_present{true};
    bool saw_only_masked_password{true};
    bool saw_expected_secret{false};
};

void push(MockHost& mock, InputEvent event) {
    mock.events[mock.event_count++] = event;
}

bool poll_input(void* context, InputEvent* output) {
    auto& mock = *static_cast<MockHost*>(context);
    if (mock.event_index >= mock.event_count) return false;
    *output = mock.events[mock.event_index++];
    return true;
}

bool get_clock(void*, char* clock, stlcs_u32 clock_cap,
               char* date, stlcs_u32 date_cap) {
    if (clock_cap < 9 || date_cap < 12) return false;
    const char t[] = "20:26:31";
    const char d[] = "Saturday, Oct 31";
    for (stlcs_u32 i = 0; i < sizeof(t); ++i) clock[i] = t[i];
    for (stlcs_u32 i = 0; i < sizeof(d) && i < date_cap; ++i) date[i] = d[i];
    date[date_cap - 1] = '\0';
    return true;
}

bool present(void* context, const Presentation* frame) {
    auto& mock = *static_cast<MockHost*>(context);
    ++mock.present_count;
    mock.last_presented_transition_alpha = frame->transition_alpha;
    for (stlcs_u32 i = 0; i < PASSWORD_CAPACITY; ++i) {
        const char c = frame->password_mask[i];
        if (c != '\0' && c != '*') mock.saw_only_masked_password = false;
    }
    return mock.allow_present;
}

AuthResult authenticate(void* context, const char* user,
                        const char* password, stlcs_u32 length) {
    auto& mock = *static_cast<MockHost*>(context);
    ++mock.auth_count;
    const char expected[] = "pass123";
    mock.saw_expected_secret = user[0] == 'U' && length == 7;
    for (stlcs_u32 i = 0; i < length && i < 7; ++i)
        if (password[i] != expected[i]) mock.saw_expected_secret = false;
    if (password[length] != '\0') mock.saw_expected_secret = false;
    return mock.next_auth;
}

bool request_desktop(void* context, const char* user) {
    auto& mock = *static_cast<MockHost*>(context);
    ++mock.desktop_count;
    return mock.allow_desktop && user[0] == 'U';
}

void yield(void* context) {
    auto& mock = *static_cast<MockHost*>(context);
    ++mock.yield_count;
    mock.monotonic_ms += 33u;
}

stlcs_u64 get_monotonic_ms(void* context) {
    return static_cast<MockHost*>(context)->monotonic_ms;
}

HostApi make_api(MockHost& mock) {
    return {ABI_VERSION, sizeof(HostApi), &mock, poll_input, get_clock,
            present, authenticate, request_desktop, yield, get_monotonic_ms};
}

InputEvent character(char c, stlcs_u32 modifiers = 0) {
    return {InputKind::KeyDown, KeyCode::Character,
            static_cast<stlcs_u32>(static_cast<unsigned char>(c)), modifiers};
}
InputEvent key(KeyCode code) {
    return {InputKind::KeyDown, code, 0u, 0u};
}

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        __builtin_printf("[FAIL] %s\n", message);
    }
}
}

int main() {
    check(stlcs_abi_version() == ABI_VERSION, "ABI version export");

    check(lock_screen_fade_alpha(0u) == 0u,
          "transition alpha begins fully black");
    check(lock_screen_fade_alpha(330u) == 127u,
          "transition alpha follows real elapsed milliseconds");
    check(lock_screen_fade_alpha(660u) == 255u,
          "transition reaches full opacity at 660 ms");

    MockHost fade_mock{};
    HostApi fade_api = make_api(fade_mock);
    Session fade_session{};
    check(stlcs_start(&fade_session, &fade_api, "Fade User"),
          "fade-test session starts");
    check(fade_mock.last_presented_transition_alpha == 0u,
          "lock screen first frame is black / opacity zero");
    for (stlcs_u32 tick = 0u; tick < 20u; ++tick)
        (void)stlcs_step(&fade_session);
    check(fade_mock.last_presented_transition_alpha < 255u,
          "lock screen transition is not forced complete by a fixed tick count");
    (void)stlcs_step(&fade_session);
    check(fade_mock.last_presented_transition_alpha == 255u &&
          fade_session.transition_complete == 1u,
          "lock-screen fade reaches full opacity at elapsed deadline");
    stlcs_stop(&fade_session);

    MockHost unavailable_mock{};
    unavailable_mock.monotonic_ms = MONOTONIC_UNAVAILABLE;
    HostApi unavailable_api = make_api(unavailable_mock);
    Session unavailable_session{};
    check(stlcs_start(&unavailable_session, &unavailable_api, "No Clock"),
          "missing monotonic clock does not block lock screen startup");
    check(unavailable_mock.last_presented_transition_alpha == 255u &&
          unavailable_session.transition_complete == 1u,
          "missing monotonic clock skips fade instead of inventing time");
    stlcs_stop(&unavailable_session);

    MockHost backwards_mock{};
    HostApi backwards_api = make_api(backwards_mock);
    Session backwards_session{};
    check(stlcs_start(&backwards_session, &backwards_api, "Backwards Clock"),
          "backwards-clock test session starts");
    backwards_mock.monotonic_ms = 999u;
    (void)stlcs_step(&backwards_session);
    check(backwards_mock.last_presented_transition_alpha == 255u &&
          backwards_session.transition_complete == 1u,
          "backwards monotonic clock skips fade rather than reversing it");
    stlcs_stop(&backwards_session);

    MockHost mock{};
    HostApi api = make_api(mock);
    Session session{};
    check(stlcs_start(&session, &api, "User"), "valid session starts");
    check(session.state == SessionState::Locked, "starts locked");
    check(mock.present_count == 1, "initial frame presented");

    // Modified key combinations must not enter the credential buffer.
    push(mock, character('x', ModifierCtrl));
    push(mock, character('a'));
    push(mock, character('b'));
    push(mock, key(KeyCode::Backspace));
    push(mock, character('c'));
    check(stlcs_step(&session) == StepResult::Continue, "input step continues");
    check(session.password_length == 2 && session.password[0] == 'a' &&
          session.password[1] == 'c', "character input, modifier rejection, backspace");
    check(mock.saw_only_masked_password, "UI receives only masked password");

    push(mock, key(KeyCode::Escape));
    stlcs_step(&session);
    check(session.password_length == 0, "escape clears password");

    // Empty Enter must not call authentication.
    push(mock, key(KeyCode::Enter));
    stlcs_step(&session);
    check(mock.auth_count == 0, "empty password is not authenticated");

    // Rejected password is wiped and keeps the screen locked.
    const char wrong[] = "wrong";
    for (char c : wrong) if (c) push(mock, character(c));
    mock.next_auth = AuthResult::Rejected;
    push(mock, key(KeyCode::Enter));
    stlcs_step(&session);
    check(mock.auth_count == 1, "authentication called for non-empty input");
    check(session.password_length == 0 && session.password[0] == 0,
          "password buffer wiped after rejected authentication");
    check(session.state == SessionState::Locked, "rejected auth remains locked");
    check(mock.desktop_count == 0, "rejected auth does not request desktop");

    // Authentication service outage also fails closed.
    const char outage[] = "retry";
    for (char c : outage) if (c) push(mock, character(c));
    mock.next_auth = AuthResult::ServiceUnavailable;
    push(mock, key(KeyCode::Enter));
    stlcs_step(&session);
    check(mock.auth_count == 2 && session.state == SessionState::Locked,
          "authentication outage fails closed");
    check(session.password_length == 0, "password wiped after auth outage");

    // Successful auth delegates the desktop transition to stlgn.
    const char good[] = "pass123";
    for (char c : good) push(mock, character(c));
    mock.next_auth = AuthResult::Accepted;
    push(mock, key(KeyCode::Enter));
    check(stlcs_step(&session) == StepResult::DesktopRequested,
          "accepted authentication requests desktop");
    check(mock.auth_count == 3 && mock.saw_expected_secret,
          "credential forwarded to authentication host");
    check(mock.desktop_count == 1, "desktop handoff requested once");
    check(session.state == SessionState::DesktopRequested,
          "session state changes after desktop handoff");

    stlcs_stop(&session);
    check(session.state == SessionState::Stopped && session.host == nullptr,
          "stop clears host binding");
    check(session.password_length == 0, "stop clears password buffer");

    // Invalid ABI/version/username must be rejected before touching services.
    MockHost invalid_mock{};
    HostApi invalid_api = make_api(invalid_mock);
    invalid_api.abi_version++;
    Session invalid_session{};
    check(!stlcs_start(&invalid_session, &invalid_api, "User"),
          "unsupported future host ABI rejected");
    invalid_api = make_api(invalid_mock);
    invalid_api.abi_version = 2u;
    check(!stlcs_start(&invalid_session, &invalid_api, "User"),
          "pre-transition HostApi ABI v2 rejected");
    invalid_api = make_api(invalid_mock);
    check(!stlcs_start(&invalid_session, &invalid_api,
                       "01234567890123456789012345678901"),
          "unterminated/oversize username rejected");

    // Presentation failure disables this component instead of pretending UI works.
    MockHost bad_present{};
    bad_present.allow_present = false;
    HostApi bad_api = make_api(bad_present);
    Session bad_session{};
    check(!stlcs_start(&bad_session, &bad_api, "User"),
          "failed first presentation rejects startup");

    __builtin_printf("Starlix stlcs state test: %s (%d failures)\n",
                     failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
