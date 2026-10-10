"""Storage and download integration scenarios."""

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
    storage_origins = call_tool("context.local_storage_origins", {})["origins"]
    assert any(item["origin"] == cookie_url.rstrip("/") for item in storage_origins)
    storage_key = next(
        item["storage_key"]
        for item in storage_origins
        if item["origin"] == cookie_url.rstrip("/")
    )
    assert call_tool("context.clear_local_storage", {"storage_key": storage_key})[
        "cleared"
    ]
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
    labelled_input = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#typed", "name": "Typed label"},
    )["elements"]
    assert len(labelled_input) == 1 and labelled_input[0]["name"] == "Typed label"
    labelledby_button = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#referenced", "name": "Reference name"},
    )["elements"]
    assert len(labelledby_button) == 1
    assert labelledby_button[0]["name"] == "Reference name"
    multi_reference = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#multi-reference", "name": "First Second"},
    )["elements"]
    assert len(multi_reference) == 1
    assert multi_reference[0]["name"] == "First Second"
    assert call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#native-progress", "role": "progressbar"},
    )["elements"]
    summary_elements = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#native-summary"},
    )["elements"]
    assert summary_elements and summary_elements[0]["role"] == "button", summary_elements
    aria_labelled_button = call_tool(
        "page.find_elements",
        {"page_id": page_id, "role": "button", "name": "ARIA label name"},
    )["elements"]
    assert len(aria_labelled_button) == 1
    alternative_image = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#alternative"},
    )["elements"]
    assert len(alternative_image) == 1 and alternative_image[0]["role"] == "img" and alternative_image[0]["name"] == "Alternative image", alternative_image
    for selector in ("#hidden", "#offscreen", "#ancestor-hidden", "#clipped"):
        assert not call_tool(
            "page.find_elements", {"page_id": page_id, "selector": selector}
        )["elements"]
    covered = call_tool(
        "page.find_elements", {"page_id": page_id, "selector": "#covered"}
    )["elements"]
    assert len(covered) == 1 and not covered[0]["actionable"]
    pointer_disabled = call_tool(
        "page.find_elements", {"page_id": page_id, "selector": "#pointer-disabled"}
    )["elements"]
    assert len(pointer_disabled) == 1 and not pointer_disabled[0]["actionable"]
    inherited_pointer_disabled = call_tool(
        "page.find_elements",
        {"page_id": page_id, "selector": "#inherited-pointer-disabled"},
    )["elements"]
    assert len(inherited_pointer_disabled) == 1
    assert not inherited_pointer_disabled[0]["actionable"]
    pointer_restored = call_tool(
        "page.find_elements", {"page_id": page_id, "selector": "#pointer-restored"}
    )["elements"]
    assert len(pointer_restored) == 1 and pointer_restored[0]["actionable"]
    assert (
        call_tool(
            "page.find_elements",
            {"page_id": page_id, "role": "heading", "text": "Fixture heading"},
        )["elements"][0]["tag"]
        == "h1"
    )
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
    assert (
        call_tool("download.wait", {"guid": started_download["download_guid"]})["state"]
        == "complete"
    )
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
    assert (
        call_tool("download.wait", {"guid": slow_download["download_guid"]})["state"]
        == "cancelled"
    )
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
