"""Page storage, content, interaction, and lifecycle integration scenarios."""

import base64
import json
import time
from http.client import HTTPConnection
from urllib.parse import urlsplit


def run_storage_download_scenarios(state):
    call_tool = state.call_tool
    request = state.request
    meta = state.meta
    endpoint = state.endpoint
    token = state.token
    fixture_server = state.fixture_server
    profile = state.profile
    upload_path = state.upload_path
    page_id = state.page_id
    cookie_url = state.cookie_url
    PROTOCOL_VERSION = state.protocol_version
    set_storage = call_tool(
        "page.set_storage",
        {
            "page_id": page_id,
            "local": {"local_key": "local_value"},
            "session": {"session_key": "session_value"},
        },
    )
    assert set_storage["local"]["local_key"] == "local_value"
    storage = call_tool("page.storage_state", {"page_id": page_id})
    assert storage["local"]["local_key"] == "local_value"
    assert storage["session"]["session_key"] == "session_value"
    storage_origins = call_tool("context.local_storage_origins", {})[
        "origins"
    ]
    assert any(item["origin"] == cookie_url.rstrip("/") for item in storage_origins)
    storage_key = next(
        item["storage_key"]
        for item in storage_origins
        if item["origin"] == cookie_url.rstrip("/")
    )
    assert call_tool(
        "context.clear_local_storage", {"storage_key": storage_key}
    )["cleared"]
    assert call_tool(
        "page.prepare_file_upload",
        {"page_id": page_id, "paths": [str(upload_path)]},
    )["ready"]
    upload_bounds = json.loads(
        call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "JSON.stringify((r => ({x:r.x+r.width/2,y:r.y+r.height/2}))(document.querySelector('#upload').getBoundingClientRect()))",
            },
        )["value"]
    )
    assert call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(upload_bounds["x"]),
            "y": round(upload_bounds["y"]),
        },
    )["clicked"]
    uploaded_name = None
    for _ in range(20):
        uploaded_name = call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "document.querySelector('#upload').files[0]?.name || null",
            },
        )["value"]
        if uploaded_name:
            break
        time.sleep(0.1)
    assert uploaded_name == upload_path.name
    found_buttons = call_tool(
        "page.find_elements",
        {"page_id": page_id, "role": "button", "name": "Click"},
    )["elements"]
    assert len(found_buttons) == 1
    assert found_buttons[0]["center_x"] > 0
    assert call_tool(
        "page.find_elements",
        {"page_id": page_id, "role": "heading", "text": "Fixture heading"},
    )["elements"][0]["tag"] == "h1"
    download_path = profile / "download-fixture.txt"
    download_url = f"http://127.0.0.1:{fixture_server.server_port}/download"
    started_download = call_tool(
        "page.download",
        {"page_id": page_id, "url": download_url, "path": str(download_path)},
    )
    assert started_download["download_guid"]
    download_item = None
    for _ in range(50):
        download_item = next(
            (
                item
                for item in call_tool("context.downloads", {})["downloads"]
                if item["guid"] == started_download["download_guid"]
            ),
            None,
        )
        if download_item and download_item["state"] == "complete":
            break
        time.sleep(0.1)
    assert download_item and download_item["state"] == "complete"
    assert call_tool(
        "download.wait", {"guid": started_download["download_guid"]}
    )["state"] == "complete"
    assert download_path.read_bytes() == b"native download fixture\n"
    slow_download = call_tool(
        "page.download",
        {
            "page_id": page_id,
            "url": f"http://127.0.0.1:{fixture_server.server_port}/slow_download",
            "path": str(profile / "slow-download.bin"),
        },
    )
    cancelled_download = call_tool(
        "download.cancel", {"guid": slow_download["download_guid"]}
    )
    assert cancelled_download["cancel_requested"]
    assert cancelled_download["cancelled"]
    assert call_tool(
        "download.wait", {"guid": slow_download["download_guid"]}
    )["state"] == "cancelled"
    target = urlsplit(endpoint)
    cancelled_wait = HTTPConnection(target.hostname, target.port, timeout=5)
    cancelled_wait.request(
        "POST",
        target.path,
        body=json.dumps(
            request(
                "tools/call",
                {
                    "name": "page.wait_for_url",
                    "arguments": {
                        "page_id": page_id,
                        "url": f"http://127.0.0.1:{fixture_server.server_port}/never",
                    },
                    **meta,
                },
            )
        ),
        headers={
            "Content-Type": "application/json",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
            "Mcp-Method": "tools/call",
            "Mcp-Name": "page.wait_for_url",
            "Authorization": f"Bearer {token}",
        },
    )
    cancelled_wait.close()
    assert call_tool("page.get_info", {"page_id": page_id})["page_id"] == page_id


def run_page_read_scenarios(state):
    call_tool = state.call_tool
    page_id = state.page_id
    accessibility = call_tool("page.accessibility_snapshot", {"page_id": page_id})
    assert accessibility["root_id"]
    assert any(
        node["role"] == "button" and node["name"] == "Click"
        for node in accessibility["nodes"]
    )
    click_accessible_node = next(
        node
        for node in accessibility["nodes"]
        if node["role"] == "button" and node["name"] == "Click"
    )
    assert click_accessible_node["description"] == "Click action"
    assert click_accessible_node["collapsed"]
    visible = call_tool("page.visible_content", {"page_id": page_id})
    assert visible["title"] == "Slow fixture"
    assert "Fixture heading" in visible["text"]
    assert visible["headings"] == [{"level": 1, "text": "Fixture heading"}]
    assert any(link["text"] == "Next" for link in visible["links"])
    call_tool("page.console_messages", {"page_id": page_id})
    initial_network_events = call_tool(
        "page.network_events", {"page_id": page_id}
    )["events"]
    main_navigation = next(
        item
        for item in initial_network_events
        if item["event_type"] == "navigation_response"
        and item["url"].endswith("/slow")
    )
    assert any(
        header["name"].lower() == "x-mcp-fixture"
        and header["value"] == "navigation-response"
        for header in main_navigation["response_headers"]
    )
    navigation_request = next(
        item
        for item in initial_network_events
        if item["event_type"] == "navigation_request"
        and item["url"].endswith("/slow")
    )
    assert navigation_request["request_headers"]
    call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "expression": "fetch('/next', {headers: {'X-Mcp-Request': 'captured'}}).then(r => r.text())",
        },
    )
    network_events = []
    for _ in range(20):
        network_events.extend(
            call_tool("page.network_events", {"page_id": page_id})["events"]
        )
        if (
            any(
                item["event_type"] == "resource_complete"
                and item["final_url"].endswith("/next")
                for item in network_events
            )
            and any(
                item["event_type"] == "subresource_request"
                and item["url"].endswith("/next")
                for item in network_events
            )
            and any(
                item["event_type"] == "subresource_response"
                and item["url"].endswith("/next")
                for item in network_events
            )
            and any(
                item["event_type"] == "subresource_response_body"
                and item["request_url"].endswith("/next")
                for item in network_events
            )
        ):
            break
        time.sleep(0.1)
    assert any(
        item["event_type"] == "resource_complete"
        and item["final_url"].endswith("/next")
        and item["http_status"] == 200
        for item in network_events
    )
    completed_fetch = next(
        item
        for item in network_events
        if item["event_type"] == "resource_complete"
        and item["final_url"].endswith("/next")
        and item["http_status"] == 200
    )
    assert completed_fetch["request_id"] > 0
    assert completed_fetch["request_destination"] >= 0
    assert completed_fetch["total_received_bytes"] > 0
    subresource_request = next(
        item
        for item in network_events
        if item["event_type"] == "subresource_request"
        and item["url"].endswith("/next")
    )
    assert any(
        header["name"].lower() == "x-mcp-request"
        and header["value"] == "captured"
        for header in subresource_request["request_headers"]
    )
    subresource_response = next(
        item
        for item in network_events
        if item["event_type"] == "subresource_response"
        and item["url"].endswith("/next")
    )
    assert any(
        header["name"].lower() == "x-mcp-fixture"
        and header["value"] == "navigation-response"
        for header in subresource_response["response_headers"]
    )
    subresource_body = next(
        item
        for item in network_events
        if item["event_type"] == "subresource_response_body"
        and item["request_id"] == subresource_request["request_id"]
    )
    decoded_subresource_body = base64.b64decode(
        subresource_body["body_base64"]
    )
    assert subresource_body["body_bytes"] == len(decoded_subresource_body)
    assert b"Slow fixture" in decoded_subresource_body
    call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "expression": "console.warn('fixture console warning')",
        },
    )
    console_messages = []
    for _ in range(20):
        console_messages.extend(
            call_tool("page.console_messages", {"page_id": page_id})["messages"]
        )
        if any("fixture console warning" in item["message"] for item in console_messages):
            break
        time.sleep(0.1)
    assert any(
        item["level"] == 2 and "fixture console warning" in item["message"]
        for item in console_messages
    )
    assert call_tool(
        "page.evaluate",
        {"page_id": page_id, "expression": "document.title"},
    )["value"] == "Slow fixture"
    frames = []
    child_frame = None
    for _ in range(20):
        frames = call_tool("page.frames", {"page_id": page_id})["frames"]
        child_frame = next(
            (
                frame
                for frame in frames
                if not frame["main_frame"]
                and frame["url"] == "about:srcdoc"
                and frame["live"]
            ),
            None,
        )
        if child_frame:
            break
        time.sleep(0.1)
    assert any(frame["main_frame"] for frame in frames)
    assert child_frame is not None
    child_evaluation = call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "frame_id": child_frame["frame_id"],
            "expression": "document.body.textContent",
        },
    )
    assert child_evaluation.get("value") == "child frame", (
        child_frame,
        child_evaluation,
    )
    assert call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "frame_id": child_frame["frame_id"],
            "expression": "location.origin",
        },
    )["value"] == "null"
    assert call_tool(
        "page.find_elements",
        {
            "page_id": page_id,
            "frame_id": child_frame["frame_id"],
            "selector": "body",
            "text": "child frame",
        },
    )["elements"]
    assert call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "expression": "document.title = 'Evaluated directly'",
        },
    )["value"] == "Evaluated directly"


def run_page_interaction_scenarios(state):
    call_tool = state.call_tool
    post = state.post
    request = state.request
    meta = state.meta
    page_id = state.page_id
    button_bounds = json.loads(
        call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "JSON.stringify((r => ({x:r.x+r.width/2,y:r.y+r.height/2}))(document.querySelector('button').getBoundingClientRect()))",
            },
        )["value"]
    )
    assert call_tool(
        "page.hover",
        {
            "page_id": page_id,
            "x": round(button_bounds["x"]),
            "y": round(button_bounds["y"]),
        },
    )["hovered"]
    hovered = False
    for _ in range(20):
        hovered = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "window.hovered"},
        )["value"]
        if hovered:
            break
        time.sleep(0.1)
    assert hovered is True
    assert call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(button_bounds["x"]),
            "y": round(button_bounds["y"]),
        },
    )["clicked"]
    clicked = False
    for _ in range(20):
        clicked = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "window.clicked"},
        )["value"]
        if clicked:
            break
        time.sleep(0.1)
    assert clicked is True
    prompt_bounds = json.loads(
        call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "JSON.stringify((r => ({x:r.x+r.width/2,y:r.y+r.height/2}))(document.querySelector('#prompt').getBoundingClientRect()))",
            },
        )["value"]
    )
    assert call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(prompt_bounds["x"]),
            "y": round(prompt_bounds["y"]),
        },
    )["clicked"]
    dialog_info = {}
    for _ in range(20):
        dialog_info = call_tool("page.dialog_info", {"page_id": page_id})
        if dialog_info["open"]:
            break
        time.sleep(0.1)
    assert dialog_info["type"] == "prompt"
    assert dialog_info["message"] == "Name?"
    assert dialog_info["default_prompt"] == "default"
    dialog_handled = False
    for _ in range(20):
        status, dialog_response = post(
            "tools/call",
            request(
                "tools/call",
                {
                    "name": "page.accept_dialog",
                    "arguments": {"page_id": page_id, "prompt_text": "MCP answer"},
                    **meta,
                },
            ),
        )
        assert status == 200
        if not dialog_response["result"].get("isError"):
            dialog_handled = dialog_response["result"]["structuredContent"]["handled"]
            break
        time.sleep(0.1)
    assert dialog_handled
    prompt_result = None
    for _ in range(20):
        prompt_result = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "window.promptResult"},
        )["value"]
        if prompt_result == "MCP answer":
            break
        time.sleep(0.1)
    assert prompt_result == "MCP answer"
    assert call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(prompt_bounds["x"]),
            "y": round(prompt_bounds["y"]),
        },
    )["clicked"]
    dialog_dismissed = False
    for _ in range(20):
        status, dialog_response = post(
            "tools/call",
            request(
                "tools/call",
                {
                    "name": "page.dismiss_dialog",
                    "arguments": {"page_id": page_id},
                    **meta,
                },
            ),
        )
        assert status == 200
        if not dialog_response["result"].get("isError"):
            dialog_dismissed = dialog_response["result"]["structuredContent"]["handled"]
            break
        time.sleep(0.1)
    assert dialog_dismissed
    assert call_tool(
        "page.evaluate",
        {"page_id": page_id, "expression": "window.promptResult"},
    )["value"] is None
    assert call_tool(
        "page.check", {"page_id": page_id, "selector": "#agree"}
    )["checked"]
    checkbox_checked = False
    for _ in range(20):
        checkbox_checked = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "document.querySelector('#agree').checked"},
        )["value"]
        if checkbox_checked:
            break
        time.sleep(0.1)
    assert checkbox_checked
    assert call_tool(
        "page.evaluate",
        {"page_id": page_id, "expression": "window.checkboxTrusted"},
    )["value"] is True
    assert not call_tool(
        "page.uncheck", {"page_id": page_id, "selector": "#agree"}
    )["checked"]
    checkbox_checked = True
    for _ in range(20):
        checkbox_checked = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "document.querySelector('#agree').checked"},
        )["value"]
        if not checkbox_checked:
            break
        time.sleep(0.1)
    assert not checkbox_checked
    assert call_tool(
        "page.focus_element", {"page_id": page_id, "selector": "#typed"}
    )["focused"]
    assert call_tool(
        "page.type_text", {"page_id": page_id, "text": "native input"}
    )["characters_sent"] == len("native input")
    typed_value = None
    for _ in range(20):
        typed_value = call_tool(
            "page.evaluate",
            {
                "page_id": page_id,
                "expression": "document.getElementById('typed').value",
            },
        )["value"]
        if typed_value == "native input":
            break
        time.sleep(0.1)
    assert typed_value == "native input"
    fill_result = call_tool(
        "page.fill",
        {
            "page_id": page_id,
            "selector": "#typed",
            "value": "filled directly",
        },
    )
    assert fill_result["filled"] and fill_result["value"] == "filled directly"
    assert call_tool(
        "page.select_option",
        {"page_id": page_id, "selector": "#choice", "value": "two"},
    )["value"] == "two"
    assert call_tool(
        "page.evaluate",
        {"page_id": page_id, "expression": "window.selected"},
    )["value"] == "two"
    assert call_tool(
        "page.focus_element", {"page_id": page_id, "selector": "#typed"}
    )["focused"]
    assert call_tool("page.press", {"page_id": page_id, "key": "Enter"})[
        "pressed"
    ]
    last_key = None
    for _ in range(20):
        last_key = call_tool(
            "page.evaluate",
            {"page_id": page_id, "expression": "window.lastKey"},
        )["value"]
        if last_key == "Enter":
            break
        time.sleep(0.1)
    assert last_key == "Enter"
    screenshot_call = request(
        "tools/call",
        {"name": "page.screenshot", "arguments": {"page_id": page_id}, **meta},
    )
    _, screenshot_response = post("tools/call", screenshot_call)
    screenshot_result = screenshot_response["result"]
    image = next(
        item for item in screenshot_result["content"] if item["type"] == "image"
    )
    png = base64.b64decode(image["data"], validate=True)
    assert image["mimeType"] == "image/png" and png.startswith(b"\x89PNG\r\n\x1a\n")
    assert screenshot_result["structuredContent"]["width"] > 0
    state.button_bounds = button_bounds


def run_final_scenarios(state):
    call_tool = state.call_tool
    post = state.post
    request = state.request
    meta = state.meta
    page_id = state.page_id
    target_url = state.target_url
    button_bounds = state.button_bounds
    audit_path = state.audit_path
    def wait_for_url(prefix):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            info = call_tool("page.get_info", {"page_id": page_id})
            if info["url"].startswith(prefix):
                return info
            time.sleep(0.1)
        raise AssertionError(f"Page did not navigate to {prefix}")

    wait_for_url(target_url)
    forward_url = "data:text/html,forward"
    call_tool(
        "page.evaluate",
        {
            "page_id": page_id,
            "expression": "window.onbeforeunload = e => { e.preventDefault(); e.returnValue = ''; }",
        },
    )
    call_tool(
        "page.click",
        {
            "page_id": page_id,
            "x": round(button_bounds["x"]),
            "y": round(button_bounds["y"]),
        },
    )
    call_tool("page.goto", {"page_id": page_id, "url": forward_url})
    beforeunload = {}
    for _ in range(20):
        beforeunload = call_tool("page.dialog_info", {"page_id": page_id})
        if beforeunload["open"]:
            break
        time.sleep(0.1)
    assert beforeunload["type"] == "beforeunload"
    assert call_tool("page.accept_dialog", {"page_id": page_id})["handled"]
    wait_for_url("data:text/html,forward")
    call_tool("page.back", {"page_id": page_id})
    call_tool("page.forward", {"page_id": page_id})
    call_tool("page.reload", {"page_id": page_id})
    call_tool("page.stop", {"page_id": page_id})
    assert call_tool("page.get_info", {"page_id": page_id})["page_id"] == page_id
    assert call_tool("page.focus", {"page_id": page_id})["focused"]
    call_tool("page.new", {})
    assert call_tool("page.close", {"page_id": page_id})["closed"]
    stale_page = request(
        "tools/call",
        {"name": "page.get_info", "arguments": {"page_id": page_id}, **meta},
    )
    _, stale_response = post("tools/call", stale_page)
    assert stale_response["result"].get("isError")
    assert post("server/discover", request("server/discover", meta), authenticate=False)[0] == 401
    assert post(
        "server/discover", request("server/discover", meta), origin="http://example.test"
    )[0] == 403
    assert post(
        "server/discover", request("server/discover", meta), header_method="tools/list"
    )[0] == 400
    deadline = time.monotonic() + 5
    while not audit_path.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    audit_events = [json.loads(line) for line in audit_path.read_text().splitlines()]
    assert any(event.get("tool") == "page.goto" for event in audit_events)
    assert all("arguments" not in event and "token" not in event for event in audit_events)
    print("Embedded MCP endpoint integration checks passed.")
