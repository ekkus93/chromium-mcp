"""Context and page initialization scenarios for the Chromium MCP integration test."""

import json
import os
import platform
import time
from http.client import HTTPConnection, IncompleteRead
from pathlib import Path
from urllib.parse import urlsplit


def run_context_scenarios(state):
    call_tool = state.call_tool
    post = state.post
    request = state.request
    meta = state.meta
    fixture_server = state.fixture_server
    if os.environ.get("CHROMIUM_MCP_SKIP_TRACE") != "1":
        assert call_tool("browser.start_trace", {"categories": "toplevel"})[
            "started"
        ]
        trace_result = call_tool("browser.stop_trace", {})
        assert trace_result["complete"]
        assert Path(trace_result["path"]).exists()

    listed_pages = call_tool("page.list", {})["pages"]
    assert listed_pages
    contexts = call_tool("browser.list_contexts", {})["contexts"]
    assert contexts and contexts[0]["context_id"]
    assert isinstance(call_tool("context.downloads", {})["downloads"], list)
    assert call_tool("context.clear_http_cache", {})["cleared"]
    assert call_tool(
        "context.set_accept_language", {"accept_language": "fr-CA,fr"}
    )["accept_language"] == "fr-CA,fr"
    assert call_tool(
        "context.set_referrers_enabled", {"enabled": False}
    )["enabled"] is False
    assert call_tool(
        "context.set_referrers_enabled", {"enabled": True}
    )["enabled"] is True
    assert call_tool("context.close_idle_connections", {})["closed"]
    cookie_url = f"http://127.0.0.1:{fixture_server.server_port}/"
    permission_types = {
        item["name"]: item["settings"]
        for item in call_tool("context.list_permissions", {})["permissions"]
    }
    assert "block" in permission_types["usb-guard"]
    assert call_tool(
        "context.set_permission",
        {"url": cookie_url, "permission": "usb-guard", "setting": "block"},
    )["setting"] == "block"
    assert call_tool(
        "context.set_permission",
        {
            "url": cookie_url,
            "permission": "geolocation",
            "setting": "allow",
        },
    )["setting"] == "allow"
    assert call_tool(
        "context.permission",
        {"url": cookie_url, "permission": "geolocation"},
    )["setting"] == "allow"
    cookie_result = call_tool(
        "context.set_cookies",
        {
            "cookies": [
                {
                    "url": cookie_url,
                    "name": "native_mcp_cookie",
                    "value": "set-through-browser-api",
                }
            ]
        },
    )
    assert cookie_result["set_count"] == 1
    regular_storage = call_tool("context.storage_state", {})
    assert isinstance(regular_storage["cookies"], list)
    assert any(
        cookie["name"] == "native_mcp_cookie"
        and cookie["value"] == "set-through-browser-api"
        for cookie in regular_storage["cookies"]
    )
    private_context = call_tool("browser.create_context", {})["context_id"]
    assert private_context != contexts[0]["context_id"]
    second_private_context = call_tool("browser.create_context", {})["context_id"]
    assert second_private_context != private_context
    assert call_tool(
        "context.set_permission",
        {
            "context_id": private_context,
            "url": cookie_url,
            "permission": "geolocation",
            "setting": "block",
        },
    )["setting"] == "block"
    assert call_tool(
        "context.permission",
        {"url": cookie_url, "permission": "geolocation"},
    )["setting"] == "allow"
    assert call_tool(
        "browser.list_contexts", {}
    )["contexts"][-1]["off_the_record"]
    assert call_tool(
        "context.set_cookies",
        {
            "context_id": private_context,
            "cookies": [
                {
                    "url": cookie_url,
                    "name": "private_mcp_cookie",
                    "value": "private-only",
                }
            ],
        },
    )["set_count"] == 1
    private_storage = call_tool(
        "context.storage_state", {"context_id": private_context}
    )
    assert isinstance(private_storage["cookies"], list)
    assert any(
        cookie["name"] == "private_mcp_cookie"
        for cookie in private_storage["cookies"]
    )
    for context_id, cookie_value in (
        (private_context, "isolated-one"),
        (second_private_context, "isolated-two"),
    ):
        assert call_tool(
            "context.set_cookies",
            {
                "context_id": context_id,
                "cookies": [{
                    "url": cookie_url,
                    "name": "same_cookie_name",
                    "value": cookie_value,
                }],
            },
        )["set_count"] == 1
    for context_id, expected_cookie in (
        (private_context, "isolated-one"),
        (second_private_context, "isolated-two"),
    ):
        cookies = call_tool("context.storage_state", {"context_id": context_id})["cookies"]
        assert any(
            item["name"] == "same_cookie_name" and item["value"] == expected_cookie
            for item in cookies
        ), cookies
    assert call_tool(
        "context.set_permission",
        {
            "context_id": second_private_context,
            "url": cookie_url,
            "permission": "geolocation",
            "setting": "allow",
        },
    )["setting"] == "allow"
    assert call_tool(
        "context.permission",
        {"context_id": private_context, "url": cookie_url, "permission": "geolocation"},
    )["setting"] == "block"
    context_pages = []
    download_guids = []
    for index, context_id in enumerate((private_context, second_private_context)):
        page = call_tool("page.new", {"context_id": context_id})["page_id"]
        context_pages.append(page)
        call_tool("page.goto", {"page_id": page, "url": cookie_url + "slow"})
        call_tool("page.wait_for_load_state", {"page_id": page})
        value = f"isolated-{index}"
        call_tool(
            "page.set_storage",
            {
                "page_id": page,
                "local": {"shared_key": value},
                "session": {"shared_key": value},
            },
        )
        download = call_tool(
            "page.download",
            {
                "page_id": page,
                "url": cookie_url + "download",
                "path": str(state.profile / f"context-{index}-download.txt"),
            },
        )
        download_guids.append(download["download_guid"])
    for index, (context_id, page) in enumerate(
        zip((private_context, second_private_context), context_pages)
    ):
        value = f"isolated-{index}"
        storage = call_tool("page.storage_state", {"page_id": page})
        assert storage["local"]["shared_key"] == value, storage
        assert storage["session"]["shared_key"] == value, storage
        assert call_tool(
            "download.wait", {"context_id": context_id, "guid": download_guids[index]}
        )["state"] == "complete"
        downloads = call_tool("context.downloads", {"context_id": context_id})[
            "downloads"
        ]
        assert any(item["guid"] == download_guids[index] for item in downloads)
        assert not any(item["guid"] == download_guids[1 - index] for item in downloads)
        pages = call_tool("page.list", {"context_id": context_id})["pages"]
        assert any(item["page_id"] == page for item in pages), pages
        assert not any(item["page_id"] == context_pages[1 - index] for item in pages)
    for page in context_pages:
        assert call_tool("page.close", {"page_id": page})["closed"]
    first_close = call_tool(
        "browser.close_context", {"context_id": private_context}
    )
    assert first_close["closed"] or first_close["close_requested"]
    for _ in range(100):
        if not any(
            item["context_id"] == private_context
            for item in call_tool("browser.list_contexts", {})["contexts"]
        ):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("first owned context remained listed")
    assert call_tool(
        "context.storage_state", {"context_id": second_private_context}
    )["cookies"]
    second_close = call_tool(
        "browser.close_context", {"context_id": second_private_context}
    )
    assert second_close["closed"] or second_close["close_requested"]
    for _ in range(100):
        if not any(
            item["context_id"] == second_private_context
            for item in call_tool("browser.list_contexts", {})["contexts"]
        ):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("second owned context remained listed")
    closed_context = request(
        "tools/call",
        {
            "name": "page.list",
            "arguments": {"context_id": private_context},
            **meta,
        },
    )
    _, closed_context_response = post("tools/call", closed_context)
    assert closed_context_response["result"].get("isError")
    assert closed_context_response["result"]["structuredContent"]["error_code"]

    window_context = call_tool("browser.create_context", {})["context_id"]
    private_page = call_tool(
        "page.new", {"context_id": window_context}
    )
    assert private_page["context_id"] == window_context
    private_url = f"http://127.0.0.1:{fixture_server.server_port}/slow"
    call_tool(
        "page.goto", {"page_id": private_page["page_id"], "url": private_url}
    )
    call_tool("page.wait_for_load_state", {"page_id": private_page["page_id"]})
    private_frames = call_tool(
        "page.frames", {"page_id": private_page["page_id"]}
    )["frames"]
    private_child_frame = next(
        (item["frame_id"] for item in private_frames if not item["main_frame"]),
        None,
    )
    assert private_child_frame
    call_tool(
        "page.evaluate",
        {
            "page_id": private_page["page_id"],
            "expression": "fetch('/next')",
        },
    )
    private_events = []
    for _ in range(30):
        private_events.extend(
            call_tool(
                "page.network_events", {"page_id": private_page["page_id"]}
            )["events"]
        )
        if any(
            event["event_type"] == "subresource_request"
            and event["url"].endswith("/next")
            for event in private_events
        ):
            break
        time.sleep(0.1)
    assert any(
        event["event_type"] == "subresource_request"
        and event["page_id"] == private_page["page_id"]
        and event["url"].endswith("/next")
        for event in private_events
    ), private_events
    call_tool(
        "page.evaluate",
        {
            "page_id": private_page["page_id"],
            "expression": "window.onbeforeunload = e => { e.preventDefault(); e.returnValue = ''; }",
        },
    )
    button = call_tool(
        "page.find_elements",
        {"page_id": private_page["page_id"], "selector": "button"},
    )["elements"][0]
    call_tool(
        "page.click",
        {
            "page_id": private_page["page_id"],
            "x": round(button["center_x"]),
            "y": round(button["center_y"]),
        },
    )
    close_result = call_tool(
        "browser.close_context", {"context_id": window_context}
    )
    assert close_result["closed"] is False and close_result["close_requested"]
    pending_dialog = None
    for _ in range(30):
        dialog = call_tool("page.dialog_info", {"page_id": private_page["page_id"]})
        if dialog["open"]:
            pending_dialog = dialog
            break
        time.sleep(0.1)
    assert pending_dialog and pending_dialog["type"] == "beforeunload"
    assert call_tool(
        "page.dismiss_dialog", {"page_id": private_page["page_id"]}
    )["handled"]
    assert any(
        item["context_id"] == window_context
        for item in call_tool("browser.list_contexts", {})["contexts"]
    )
    if os.environ.get("CHROMIUM_MCP_TEST_CLOSE_TIMEOUT") == "1":
        time.sleep(61)
        assert any(
            item["context_id"] == window_context
            for item in call_tool("browser.list_contexts", {})["contexts"]
        )
        assert call_tool(
            "page.get_info", {"page_id": private_page["page_id"]}
        )["page_id"] == private_page["page_id"]
    call_tool(
        "page.evaluate",
        {"page_id": private_page["page_id"], "expression": "window.onbeforeunload = null"},
    )
    close_result = call_tool("browser.close_context", {"context_id": window_context})
    assert close_result["closed"] or close_result["close_requested"]
    for _ in range(100):
        listed = call_tool("browser.list_contexts", {})["contexts"]
        if not any(item["context_id"] == window_context for item in listed):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("owned context remained listed after closure")
    stale_private_page = request(
        "tools/call",
        {
            "name": "page.get_info",
            "arguments": {"page_id": private_page["page_id"]},
            **meta,
        },
    )
    _, stale_private_page_response = post("tools/call", stale_private_page)
    assert stale_private_page_response["result"].get("isError")
    assert stale_private_page_response["result"]["structuredContent"]["error_code"]
    for stale_tool, arguments in (
        ("page.network_events", {"page_id": private_page["page_id"]}),
        ("page.console_messages", {"page_id": private_page["page_id"]}),
        ("page.evaluate", {
            "page_id": private_page["page_id"],
            "frame_id": private_child_frame,
            "expression": "document.title",
        }),
    ):
        _, stale_response = post(
            "tools/call",
            request("tools/call", {
                "name": stale_tool,
                "arguments": arguments,
                **meta,
            }),
        )
        assert stale_response["result"].get("isError"), (
            stale_tool, stale_response
        )
    stale_window_context = request(
        "tools/call",
        {
            "name": "page.list",
            "arguments": {"context_id": window_context},
            **meta,
        },
    )
    _, closed_window_context_response = post(
        "tools/call", stale_window_context
    )
    assert closed_window_context_response["result"].get("isError")
    state.cookie_url = cookie_url


def run_page_setup_scenarios(state):
    call_tool = state.call_tool
    endpoint = state.endpoint
    token = state.token
    fixture_server = state.fixture_server
    ACCEPT_LANGUAGE_HEADERS = state.accept_language_headers

    event_target = urlsplit(endpoint)
    event_connection = HTTPConnection(
        event_target.hostname, event_target.port, timeout=10
    )
    event_connection.request(
        "POST",
        event_target.path,
        body=json.dumps({
            "jsonrpc": "2.0",
            "id": "browser-events-1",
            "method": "subscriptions/listen",
            "params": {
                "notifications": {
                    "resourceSubscriptions": [
                        "chromium-mcp://browser/events",
                        "chromium-mcp://browser/unavailable",
                    ]
                },
                **state.meta,
            },
        }),
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": state.protocol_version,
            "Mcp-Method": "subscriptions/listen",
            "Authorization": f"Bearer {token}",
            "Host": event_target.netloc,
        },
    )
    event_response = event_connection.getresponse()
    assert event_response.status == 200
    assert event_response.getheader("Content-Type") == "text/event-stream"

    def next_event():
        while True:
            line = event_response.readline()
            assert line, "MCP event stream closed unexpectedly"
            if line.startswith(b"data: "):
                return json.loads(line[len(b"data: ") :])

    acknowledged = next_event()
    assert acknowledged["method"] == "notifications/subscriptions/acknowledged"
    assert acknowledged["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == "browser-events-1", acknowledged
    assert acknowledged["params"]["notifications"]["resourceSubscriptions"] == [
        "chromium-mcp://browser/events"
    ]
    second_connection = HTTPConnection(
        event_target.hostname, event_target.port, timeout=10
    )
    second_connection.request(
        "POST",
        event_target.path,
        body=json.dumps({
            "jsonrpc": "2.0",
            "id": "browser-events-2",
            "method": "subscriptions/listen",
            "params": {
                "notifications": {
                    "resourceSubscriptions": ["chromium-mcp://browser/events"]
                },
                **state.meta,
            },
        }),
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": state.protocol_version,
            "Mcp-Method": "subscriptions/listen",
            "Authorization": f"Bearer {token}",
            "Host": event_target.netloc,
        },
    )
    second_response = second_connection.getresponse()
    assert second_response.status == 200

    def second_event():
        while True:
            line = second_response.readline()
            assert line, "Second MCP event stream closed unexpectedly"
            if line.startswith(b"data: "):
                return json.loads(line[len(b"data: ") :])

    second_acknowledged = second_event()
    assert second_acknowledged["method"] == "notifications/subscriptions/acknowledged"
    assert second_acknowledged["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == "browser-events-2"
    unsupported_connection = HTTPConnection(
        event_target.hostname, event_target.port, timeout=10
    )
    unsupported_connection.request(
        "POST",
        event_target.path,
        body=json.dumps({
            "jsonrpc": "2.0",
            "id": "unsupported-listener",
            "method": "subscriptions/listen",
            "params": {
                "notifications": {
                    "resourceSubscriptions": ["chromium-mcp://browser/unknown"],
                    "toolsListChanged": True,
                },
                **state.meta,
            },
        }),
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": state.protocol_version,
            "Mcp-Method": "subscriptions/listen",
            "Authorization": f"Bearer {token}",
            "Host": event_target.netloc,
        },
    )
    unsupported_response = unsupported_connection.getresponse()
    assert unsupported_response.status == 200
    while True:
        line = unsupported_response.readline()
        assert line, "unsupported listener closed before acknowledgment"
        if line.startswith(b"data: "):
            unsupported_ack = json.loads(line[6:])
            break
    assert unsupported_ack["params"]["notifications"] == {
        "resourceSubscriptions": []
    }, unsupported_ack
    event_cursor = 0

    def next_browser_event(event_type, page_id=None):
        nonlocal event_cursor
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            _, resource = state.post(
                "resources/read",
                state.request("resources/read", {
                    "uri": f"chromium-mcp://browser/events?after={event_cursor}",
                    **state.meta,
                }),
            )
            journal = json.loads(resource["result"]["contents"][0]["text"])
            event_cursor = int(journal["next_cursor"])
            for entry in journal["events"]:
                event = entry["event"]
                entry_cursor = int(entry["cursor"])
                if event.get("event_type") == event_type and (
                    page_id is None or event.get("page_id") == page_id
                ):
                    event_cursor = entry_cursor
                    return event
                event_cursor = max(event_cursor, entry_cursor)
            event_cursor = max(event_cursor, int(journal["next_cursor"]))
            update = next_event()
            assert update["method"] == "notifications/resources/updated"
            assert update["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == "browser-events-1"
        raise AssertionError(f"Timed out waiting for {event_type} on {page_id}")

    new_page = call_tool("page.new", {})
    page_id = new_page["page_id"]
    assert next_browser_event("tab_created", page_id)["page_id"] == page_id
    second_update = second_event()
    assert second_update["method"] == "notifications/resources/updated"
    assert second_update["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == "browser-events-2"
    unsupported_connection.sock.settimeout(0.2)
    try:
        while True:
            line = unsupported_response.readline()
            assert not line.startswith(b"data: "), line
            if not line:
                raise AssertionError("unsupported listener closed unexpectedly")
    except TimeoutError:
        pass
    unsupported_connection.close()
    target_url = f"http://127.0.0.1:{fixture_server.server_port}/slow"
    assert call_tool("page.goto", {"page_id": page_id, "url": target_url})
    assert call_tool(
        "page.wait_for_url", {"page_id": page_id, "url": target_url}
    )["url"] == target_url
    assert call_tool("page.wait_for_load_state", {"page_id": page_id})[
        "load_state"
    ] == "load"
    assert next_browser_event("navigation_committed", page_id)["url"] == target_url
    assert next_browser_event("navigation_finished", page_id)["url"] == target_url
    popup_bounds = json.loads(
        call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "JSON.stringify((r => ({x:r.x+r.width/2,y:r.y+r.height/2}))(document.querySelector('#popup').getBoundingClientRect()))",
            },
        )["value"]
    )
    assert call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(popup_bounds["x"]),
            "y": round(popup_bounds["y"]),
        },
    )["clicked"]
    popup_page_id = next_browser_event("popup_created")["page_id"]
    assert call_tool("page.close", {"page_id": popup_page_id})["closed"]
    next_browser_event("popup_closed", popup_page_id)
    closing_page_id = call_tool("page.new", {})["page_id"]
    next_browser_event("tab_created", closing_page_id)
    assert call_tool("page.close", {"page_id": closing_page_id})["closed"]
    next_browser_event("tab_closed", closing_page_id)
    large_id = "slow-listener-" + "x" * (245 * 1024)
    slow_connection = HTTPConnection(event_target.hostname, event_target.port, timeout=10)
    slow_connection.request(
        "POST",
        event_target.path,
        body=json.dumps({
            "jsonrpc": "2.0",
            "id": large_id,
            "method": "subscriptions/listen",
            "params": {
                "notifications": {
                    "resourceSubscriptions": ["chromium-mcp://browser/events"]
                },
                **state.meta,
            },
        }),
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": state.protocol_version,
            "Mcp-Method": "subscriptions/listen",
            "Authorization": f"Bearer {token}",
            "Host": event_target.netloc,
        },
    )
    slow_response = slow_connection.getresponse()
    assert slow_response.status == 200
    while True:
        line = slow_response.readline()
        assert line
        if line.startswith(b"data: "):
            assert json.loads(line[6:])["method"] == (
                "notifications/subscriptions/acknowledged"
            )
            break
    slow_page = call_tool("page.new", {})["page_id"]
    try:
        while True:
            line = slow_response.readline()
            if not line:
                break
            assert not line.startswith(b"data: "), "exhausted listener sent an update"
    except IncompleteRead:
        pass
    slow_connection.close()
    assert call_tool("page.close", {"page_id": slow_page})["closed"]
    _, before_overrun = state.post(
        "resources/read",
        state.request("resources/read", {
            "uri": "chromium-mcp://browser/events",
            **state.meta,
        }),
    )
    cursor_before_overrun = int(json.loads(
        before_overrun["result"]["contents"][0]["text"]
    )["next_cursor"])
    overrun_page = call_tool("page.new", {})["page_id"]
    for _ in range(30):
        _, watch_read = state.post(
            "resources/read",
            state.request("resources/read", {
                "uri": f"chromium-mcp://browser/events?after={cursor_before_overrun}",
                **state.meta,
            }),
        )
        watch_journal = json.loads(watch_read["result"]["contents"][0]["text"])
        if any(
            entry["event"].get("event_type") == "tab_created"
            and entry["event"].get("page_id") == overrun_page
            for entry in watch_journal["events"]
        ):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("overrun page was not observed")
    for index in range(130):
        call_tool(
            "page.goto",
            {"page_id": overrun_page, "url": f"data:text/html,overrun-{index}"},
        )
    overrun_journal = None
    for _ in range(50):
        _, overrun_read = state.post(
            "resources/read",
            state.request("resources/read", {
                "uri": f"chromium-mcp://browser/events?after={cursor_before_overrun}",
                **state.meta,
            }),
        )
        overrun_journal = json.loads(overrun_read["result"]["contents"][0]["text"])
        if int(overrun_journal["next_cursor"]) - cursor_before_overrun > 256:
            break
        time.sleep(0.1)
    assert overrun_journal["cursor_expired"], overrun_journal
    assert overrun_journal["oldest_cursor"] > cursor_before_overrun + 1
    assert call_tool("page.close", {"page_id": overrun_page})["closed"]
    event_connection.close()
    second_connection.close()
    assert any(
        value.lower().startswith("fr-ca") for value in ACCEPT_LANGUAGE_HEADERS
    ), ACCEPT_LANGUAGE_HEADERS
    call_tool(
        "page.set_viewport_size",
        {"page_id": page_id, "width": 640, "height": 480},
    )
    viewport_size = call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "expression": "[window.innerWidth, window.innerHeight]",
        },
    )["value"]
    assert viewport_size == [640, 480], viewport_size
    call_tool(
        "page.set_user_agent",
        {"page_id": page_id, "user_agent": "ChromiumMCPFixture/1.0"},
    )
    call_tool("page.reload", {"page_id": page_id})
    call_tool("page.wait_for_load_state", {"page_id": page_id})
    assert call_tool(
        "page.evaluate",
        {"page_id": page_id, "expression": "navigator.userAgent"},
    )["value"] == "ChromiumMCPFixture/1.0"
    call_tool("page.set_user_agent", {"page_id": page_id, "user_agent": ""})
    call_tool("page.reload", {"page_id": page_id})
    call_tool("page.wait_for_load_state", {"page_id": page_id})
    expected_os_permission_denial = os.environ.get(
        "CHROMIUM_MCP_EXPECT_OS_PERMISSION_DENIAL"
    )
    supported_device_permissions = {"camera", "geolocation", "microphone"}
    if expected_os_permission_denial and (
        expected_os_permission_denial not in supported_device_permissions
    ):
        raise AssertionError(
            "CHROMIUM_MCP_EXPECT_OS_PERMISSION_DENIAL must be camera, "
            "geolocation, or microphone"
        )
    permission_to_check = expected_os_permission_denial or "geolocation"
    if expected_os_permission_denial:
        browser_permission = call_tool(
            "context.set_permission",
            {
                "url": target_url,
                "permission": permission_to_check,
                "setting": "allow",
            },
        )
        assert browser_permission["setting"] == "allow"
    permission_status = call_tool(
        "page.permission_status",
        {"page_id": page_id, "permission": permission_to_check},
    )
    assert permission_status["browser_status"] in {"granted", "denied", "ask"}
    assert permission_status["combined_status"] in {"granted", "denied", "ask"}
    assert permission_status["has_device_status"] is True
    if expected_os_permission_denial:
        evidence = {
            "platform": platform.platform(),
            "permission": permission_to_check,
            **permission_status,
        }
        print(
            "OS_PERMISSION_DENIAL_EVIDENCE="
            + json.dumps(evidence, sort_keys=True),
            flush=True,
        )
        assert permission_status["browser_status"] == "granted", evidence
        assert permission_status["combined_status"] == "denied", evidence
        assert permission_status["os_or_device_denied"] is True, evidence
    if (
        permission_status["browser_status"] != "denied"
        and permission_status["combined_status"] == "denied"
    ):
        assert permission_status["os_or_device_denied"] is True
    blocked_browser_permission = call_tool(
        "context.set_permission",
        {
            "url": target_url,
            "permission": "geolocation",
            "setting": "block",
        },
    )
    assert blocked_browser_permission["setting"] == "block"
    browser_denial_status = call_tool(
        "page.permission_status",
        {"page_id": page_id, "permission": "geolocation"},
    )
    assert browser_denial_status["browser_status"] == "denied"
    assert browser_denial_status["os_or_device_denied"] is False
    call_tool(
        "context.set_permission",
        {
            "url": target_url,
            "permission": "geolocation",
            "setting": "allow",
        },
    )
    state.page_id = page_id
    state.target_url = target_url
    state.permission_to_check = permission_to_check
