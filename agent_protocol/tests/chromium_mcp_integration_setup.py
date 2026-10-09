"""Context and page initialization scenarios for the Chromium MCP integration test."""

import json
import os
import platform
import time
from http.client import HTTPConnection
from pathlib import Path
from urllib.parse import urlsplit


def run_context_scenarios(state):
    call_tool = state.call_tool
    post = state.post
    request = state.request
    meta = state.meta
    fixture_server = state.fixture_server
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
    assert call_tool(
        "browser.close_context", {"context_id": private_context}
    )["closed"]
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
    assert call_tool(
        "browser.close_context", {"context_id": window_context}
    )["closed"]
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
        "GET",
        event_target.path,
        headers={
            "Accept": "text/event-stream",
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

    opened_event = next_event()
    assert opened_event["method"] == "notifications/message"
    assert opened_event["params"]["data"]["event_type"] == "event_stream_opened"
    new_page = call_tool("page.new", {})
    page_id = new_page["page_id"]
    created_event = next_event()
    assert created_event["params"]["data"]["event_type"] == "tab_created"
    assert created_event["params"]["data"]["page_id"] == page_id
    target_url = f"http://127.0.0.1:{fixture_server.server_port}/slow"
    assert call_tool("page.goto", {"page_id": page_id, "url": target_url})
    assert call_tool(
        "page.wait_for_url", {"page_id": page_id, "url": target_url}
    )["url"] == target_url
    assert call_tool("page.wait_for_load_state", {"page_id": page_id})[
        "load_state"
    ] == "load"
    navigation_events = set()
    event_deadline = time.monotonic() + 10
    while (
        "navigation_finished" not in navigation_events
        and time.monotonic() < event_deadline
    ):
        event = next_event()["params"]["data"]
        if event["page_id"] == page_id and event["url"] == target_url:
            navigation_events.add(event["event_type"])
    assert "navigation_committed" in navigation_events, navigation_events
    assert "navigation_finished" in navigation_events, navigation_events
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
    popup_events = {}
    popup_deadline = time.monotonic() + 10
    while (
        "popup_created" not in popup_events.values()
        and time.monotonic() < popup_deadline
    ):
        event = next_event()["params"]["data"]
        popup_events[event.get("page_id")] = event["event_type"]
    popup_page_id = next(
        key for key, value in popup_events.items() if value == "popup_created"
    )
    assert popup_events[popup_page_id] == "popup_created"
    assert call_tool("page.close", {"page_id": popup_page_id})["closed"]
    popup_closed_events = set()
    popup_close_deadline = time.monotonic() + 10
    while (
        "popup_closed" not in popup_closed_events
        and time.monotonic() < popup_close_deadline
    ):
        event = next_event()["params"]["data"]
        if event.get("page_id") == popup_page_id:
            popup_closed_events.add(event["event_type"])
    assert "popup_closed" in popup_closed_events
    closing_page_id = call_tool("page.new", {})["page_id"]
    while True:
        event = next_event()["params"]["data"]
        if (
            event["event_type"] == "tab_created"
            and event["page_id"] == closing_page_id
        ):
            break
    assert call_tool("page.close", {"page_id": closing_page_id})["closed"]
    while True:
        event = next_event()["params"]["data"]
        if (
            event["event_type"] == "tab_closed"
            and event["page_id"] == closing_page_id
        ):
            break
    event_connection.close()
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
