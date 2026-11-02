"""OpenThread lock around GUARD_CHECKED_CALL (esp_br_web.c, patch 2).

The six Web GUI handlers that change Thread state take the OpenThread lock themselves (upstream code). The guard must
not hold the lock around them: ESP-IDF's platform UDP (otPlatUdpSocket, reached when Thread comes up) drops the task
switching lock exactly once while it waits for the lwIP thread, so a lock held twice stays held, and the lwIP thread,
which needs it to send into the Thread interface, deadlocks with the web server (Form network froze the board). The
guard checks the mode under the lock and lets go before the call, as for scan and join.
"""

import re

from device_js import REPO

SRC = REPO / "p4" / "components" / "esp_ot_br_server" / "src"
WEB = SRC / "esp_br_web.c"
WEB_API = SRC / "esp_br_web_api.c"

GUARDED = [
    "handle_openthread_form_network_request",
    "handle_openthread_add_network_prefix_request",
    "handle_openthread_delete_network_prefix_request",
    "handle_openthread_network_commission_request",
    "handle_openthread_add_ipaddr_request",
    "handle_openthread_delete_ipaddr_request",
]


def strip_comments_and_strings(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def closing(text, start, opening, close):
    """Index just past the bracket that closes the one before `start`."""
    depth, i = 1, start
    while depth:
        depth += {opening: 1, close: -1}.get(text[i], 0)
        i += 1
    return i


def web():
    return strip_comments_and_strings(WEB.read_text())


def guarded_calls():
    """Names of the handle_* functions called inside GUARD_CHECKED_CALL(...)."""
    text = web()
    names = []
    for match in re.finditer(r"(?<!#define )\bGUARD_CHECKED_CALL\(", text):
        names += re.findall(
            r"\b(handle_\w+)\(",
            text[match.end() : closing(text, match.end(), "(", ")")],
        )
    return names


def function_body(source, name):
    match = re.search(r"^[\w \*]*\b" + name + r"\([^)]*\)\s*\{", source, re.MULTILINE)
    assert match, f"{name} not defined in {WEB_API.name}"
    return source[match.end() : closing(source, match.end(), "{", "}")]


def test_thread_changing_gui_handlers_run_through_the_guard():
    # On reading esp_br_web.c: the six GUI handlers that change Thread state are called through GUARD_CHECKED_CALL
    assert sorted(guarded_calls()) == sorted(GUARDED)


def test_guard_holds_no_lock_around_the_call():
    # On a guarded call: the mode is checked under the lock, which is released before the call and not taken by the guard
    match = re.search(
        r"#define GUARD_CHECKED_CALL\(.*?\)((?:[^\n]*\\\n)*[^\n]*)", web()
    )
    assert match, "GUARD_CHECKED_CALL not defined"
    body = match.group(1)
    assert "guard_check_thread_op()" in body
    assert not re.search(
        r"\b(guard_lock|esp_openthread_lock_acquire|esp_openthread_lock_release)\b",
        body,
    )


def test_guarded_handlers_take_the_lock_themselves():
    # On a guarded call: the upstream handler takes and releases the OpenThread lock itself (unchanged vendored code)
    api = strip_comments_and_strings(WEB_API.read_text())
    for name in GUARDED:
        body = function_body(api, name)
        assert re.search(r"\besp_openthread_lock_acquire\b", body), name
        assert re.search(r"\besp_openthread_lock_release\b", body), name
