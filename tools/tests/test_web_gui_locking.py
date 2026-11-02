"""OpenThread lock depth under GUARD_LOCKED_CALL (esp_br_web.c, patch 2).

The guard takes the OpenThread lock around the handle_* call. ESP-IDF's platform UDP (otPlatUdpSocket, reached when
Thread comes up) drops the task switching lock exactly once while it waits for the lwIP thread; a handle_* that takes the
lock again leaves it held, and the lwIP thread, which needs it to send into the Thread interface, deadlocks with the
web server. So a function called under the guard must not take the lock itself.
"""

import re

from device_js import REPO

SRC = REPO / "p4" / "components" / "esp_ot_br_server" / "src"
WEB = SRC / "esp_br_web.c"
WEB_API = SRC / "esp_br_web_api.c"


def strip_comments_and_strings(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def guarded_calls():
    """Names of the handle_* functions called inside GUARD_LOCKED_CALL(...)."""
    web = strip_comments_and_strings(WEB.read_text())
    names = []
    for match in re.finditer(r"\bGUARD_LOCKED_CALL\(", web):
        if web[: match.start()].rstrip().endswith("#define"):
            continue
        depth, i = 1, match.end()
        while depth:
            depth += {"(": 1, ")": -1}.get(web[i], 0)
            i += 1
        names += re.findall(r"\b(handle_\w+)\(", web[match.end() : i])
    return names


def function_body(source, name):
    match = re.search(r"^[\w \*]*\b" + name + r"\([^)]*\)\s*\{", source, re.M)
    assert match, f"{name} not defined in {WEB_API.name}"
    depth, i = 1, match.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
    return source[match.end() : i]


def test_guard_covers_the_thread_changing_gui_handlers():
    # On reading esp_br_web.c: the six GUI handlers that change Thread state run under the guard
    assert sorted(guarded_calls()) == sorted(
        [
            "handle_openthread_form_network_request",
            "handle_openthread_add_network_prefix_request",
            "handle_openthread_delete_network_prefix_request",
            "handle_openthread_network_commission_request",
            "handle_openthread_add_ipaddr_request",
            "handle_openthread_delete_ipaddr_request",
        ]
    )


def test_guarded_handlers_do_not_take_the_lock_again():
    # On a call under GUARD_LOCKED_CALL: the handler holds the guard's lock once and never acquires or releases it itself
    api = strip_comments_and_strings(WEB_API.read_text())
    nested = [
        name
        for name in guarded_calls()
        if re.search(r"\besp_openthread_lock_(acquire|release)\b", function_body(api, name))
    ]
    assert nested == []
