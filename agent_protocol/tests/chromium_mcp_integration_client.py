"""MCP HTTP client setup and protocol contract checks for integration tests."""

import json
import urllib.error
import urllib.request


PROTOCOL_VERSION = "2026-07-28"


def create_mcp_client(endpoint, token):
    """Validate transport/tool contracts and return helpers used by scenarios."""
    get_request = urllib.request.Request(
        endpoint, headers={"Authorization": f"Bearer {token}"}, method="GET"
    )
    try:
        urllib.request.urlopen(get_request, timeout=10)
        raise AssertionError("standalone GET stream was accepted")
    except urllib.error.HTTPError as error:
        assert error.code == 405

    def post(
        method,
        message,
        *,
        authenticate=True,
        origin=None,
        header_method=None,
        protocol_version=None,
        header_name=None,
    ):
        headers = {
            "Content-Type": "application/json",
            "MCP-Protocol-Version": protocol_version or PROTOCOL_VERSION,
            "Mcp-Method": header_method or method,
        }
        if authenticate:
            headers["Authorization"] = f"Bearer {token}"
        if method == "tools/call":
            headers["Mcp-Name"] = header_name or message["params"]["name"]
        if method == "resources/read":
            headers["Mcp-Name"] = header_name or message["params"]["uri"]
        if origin:
            headers["Origin"] = origin
        request = urllib.request.Request(
            endpoint,
            data=json.dumps(message).encode(),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode(errors="replace")

    meta = {
        "_meta": {
            "io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION,
            "io.modelcontextprotocol/clientCapabilities": {},
        }
    }

    def request(method, params):
        return {
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params,
        }

    discover = request("server/discover", meta)
    assert post("server/discover", discover)[0] == 200
    version_call = request(
        "tools/call", {"name": "browser.version", "arguments": {}, **meta}
    )
    encoded_tool_name = post(
        "tools/call", version_call, header_name="=?base64?YnJvd3Nlci52ZXJzaW9u?="
    )
    assert encoded_tool_name[0] == 200
    assert not encoded_tool_name[1]["result"].get("isError")
    malformed_encoded_name = post(
        "tools/call", version_call, header_name="=?base64?%%%?="
    )
    assert malformed_encoded_name[0] == 400
    assert json.loads(malformed_encoded_name[1])["error"]["code"] == -32020
    status, unknown_method = post("made/up", request("made/up", meta))
    assert status == 404
    assert json.loads(unknown_method)["error"]["code"] == -32601
    status, resources = post("resources/list", request("resources/list", meta))
    assert status == 200
    resource_uris = {item["uri"] for item in resources["result"]["resources"]}
    assert "chromium-mcp://browser/version" in resource_uris
    assert "chromium-mcp://browser/contexts" in resource_uris
    assert "chromium-mcp://browser/events" in resource_uris
    status, version_resource = post(
        "resources/read",
        request(
            "resources/read",
            {"uri": "chromium-mcp://browser/version", **meta},
        ),
    )
    assert status == 200
    assert json.loads(version_resource["result"]["contents"][0]["text"])["version"]
    status, missing_resource = post(
        "resources/read",
        request("resources/read", {"uri": "chromium-mcp://browser/missing", **meta}),
    )
    assert status == 200 and missing_resource["error"]["code"] == -32602
    status, invalid_version = post(
        "tools/list", {**request("tools/list", meta), "jsonrpc": "1.0"}
    )
    invalid_version = json.loads(invalid_version)
    assert status == 400 and invalid_version["error"]["code"] == -32600
    status, missing_id = post(
        "tools/list", {"jsonrpc": "2.0", "method": "tools/list", "params": meta}
    )
    missing_id = json.loads(missing_id)
    assert status == 400 and missing_id["id"] is None
    assert missing_id["error"]["code"] == -32600
    malformed = urllib.request.Request(
        endpoint,
        data=b"{",
        headers={
            "Content-Type": "application/json",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
            "Mcp-Method": "tools/list",
            "Authorization": f"Bearer {token}",
        },
        method="POST",
    )
    try:
        urllib.request.urlopen(malformed, timeout=30)
        raise AssertionError("malformed JSON was accepted")
    except urllib.error.HTTPError as error:
        assert error.code == 400
        assert json.loads(error.read())["error"]["code"] == -32700
    wrong_content_type = urllib.request.Request(
        endpoint,
        data=json.dumps(request("tools/list", meta)).encode(),
        headers={
            "Content-Type": "text/plain",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
            "Mcp-Method": "tools/list",
            "Authorization": f"Bearer {token}",
        },
        method="POST",
    )
    try:
        urllib.request.urlopen(wrong_content_type, timeout=10)
        raise AssertionError("non-JSON content type was accepted")
    except urllib.error.HTTPError as error:
        assert error.code == 400
        assert json.loads(error.read())["error"]["code"] == -32600
    missing_version_header = urllib.request.Request(
        endpoint,
        data=json.dumps(request("tools/list", meta)).encode(),
        headers={
            "Content-Type": "application/json",
            "Mcp-Method": "tools/list",
            "Authorization": f"Bearer {token}",
        },
        method="POST",
    )
    try:
        urllib.request.urlopen(missing_version_header, timeout=10)
        raise AssertionError("request without a protocol header was accepted")
    except urllib.error.HTTPError as error:
        assert error.code == 400
        assert json.loads(error.read())["error"]["code"] == -32020
    missing_metadata = request("tools/list", {})
    status, missing_meta_response = post("tools/list", missing_metadata)
    assert status == 400
    assert json.loads(missing_meta_response)["error"]["code"] == -32602
    malformed_identity_meta = {
        "_meta": {
            "io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION,
            "io.modelcontextprotocol/clientCapabilities": {},
            "io.modelcontextprotocol/clientInfo": {"name": 7, "version": "1"},
        }
    }
    status, malformed_identity = post(
        "tools/list", request("tools/list", malformed_identity_meta)
    )
    assert status == 400
    assert json.loads(malformed_identity)["error"]["code"] == -32602
    status, mismatched_method = post(
        "tools/list", request("tools/list", meta), header_method="tools/call"
    )
    assert status == 400
    assert json.loads(mismatched_method)["error"]["code"] == -32020
    status, mismatched_name = post(
        "tools/call", version_call, header_name="browser.not_version"
    )
    assert status == 400
    assert json.loads(mismatched_name)["error"]["code"] == -32020
    unsupported_meta = {
        "_meta": {
            "io.modelcontextprotocol/protocolVersion": "2025-11-25",
            "io.modelcontextprotocol/clientCapabilities": {},
        }
    }
    status, unsupported_version = post(
        "tools/list",
        request("tools/list", unsupported_meta),
        protocol_version="2025-11-25",
    )
    assert status == 400
    unsupported_error = json.loads(unsupported_version)["error"]
    assert unsupported_error["code"] == -32022
    assert unsupported_error["data"]["supportedVersions"] == [PROTOCOL_VERSION]
    status, listing = post("tools/list", request("tools/list", meta))
    assert status == 200
    tools = {tool["name"]: tool for tool in listing["result"]["tools"]}
    expected_tools = {
        "browser.version",
        "browser.list_contexts",
        "browser.create_context",
        "browser.close_context",
        "context.storage_state",
        "context.clear_http_cache",
        "context.set_accept_language",
        "context.set_referrers_enabled",
        "context.close_idle_connections",
        "context.local_storage_origins",
        "context.clear_local_storage",
        "context.set_cookies",
        "context.downloads",
        "context.permission",
        "context.list_permissions",
        "context.set_permission",
        "download.cancel",
        "download.wait",
        "page.list",
        "page.new",
        "page.get_info",
        "page.set_viewport_size",
        "page.screenshot",
        "page.download",
        "page.prepare_file_upload",
        "page.permission_status",
        "page.accessibility_snapshot",
        "page.visible_content",
        "page.storage_state",
        "page.set_storage",
        "page.console_messages",
        "page.network_events",
        "page.frames",
        "page.find_elements",
        "page.wait_for_load_state",
        "page.wait_for_url",
        "page.evaluate",
        "page.fill",
        "page.focus_element",
        "page.select_option",
        "page.click",
        "page.hover",
        "page.check",
        "page.uncheck",
        "page.type_text",
        "page.press",
        "page.goto",
        "page.back",
        "page.forward",
        "page.reload",
        "page.stop",
        "page.close",
        "page.focus",
        "page.set_user_agent",
        "page.accept_dialog",
        "page.dialog_info",
        "page.dismiss_dialog",
        "browser.start_trace",
        "browser.stop_trace",
    }
    assert expected_tools <= tools.keys(), expected_tools - tools.keys()
    assert "browser.version" in tools
    assert "browser.start_trace" in tools and "browser.stop_trace" in tools
    assert tools["page.goto"]["inputSchema"]["required"] == ["page_id", "url"]
    assert tools["page.wait_for_load_state"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.wait_for_url"]["inputSchema"]["required"] == [
        "page_id",
        "url",
    ]
    assert tools["page.frames"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.find_elements"]["inputSchema"]["required"] == ["page_id"]
    assert "frame_id" in tools["page.find_elements"]["inputSchema"]["properties"]
    assert tools["page.storage_state"]["inputSchema"]["required"] == ["page_id"]
    assert "frame_id" in tools["page.storage_state"]["inputSchema"]["properties"]
    assert tools["page.set_storage"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.download"]["inputSchema"]["required"] == [
        "page_id",
        "url",
    ]
    assert tools["page.accessibility_snapshot"]["inputSchema"]["required"] == [
        "page_id"
    ]
    assert tools["page.visible_content"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.console_messages"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.evaluate"]["inputSchema"]["required"] == [
        "page_id",
        "expression",
    ]
    assert tools["page.fill"]["inputSchema"]["required"] == [
        "page_id",
        "selector",
        "value",
    ]
    assert tools["page.focus_element"]["inputSchema"]["required"] == [
        "page_id",
        "selector",
    ]
    assert tools["page.set_user_agent"]["inputSchema"]["required"] == [
        "page_id",
        "user_agent",
    ]
    assert tools["page.set_viewport_size"]["inputSchema"]["required"] == [
        "page_id",
        "width",
        "height",
    ]
    assert tools["page.select_option"]["inputSchema"]["required"] == [
        "page_id",
        "selector",
        "value",
    ]
    assert tools["page.click"]["inputSchema"]["required"] == [
        "page_id",
        "x",
        "y",
    ]
    assert tools["page.hover"]["inputSchema"]["required"] == [
        "page_id",
        "x",
        "y",
    ]
    for name in ("page.check", "page.uncheck"):
        assert tools[name]["inputSchema"]["required"] == [
            "page_id",
            "selector",
        ]
    assert tools["page.type_text"]["inputSchema"]["required"] == [
        "page_id",
        "text",
    ]
    assert tools["page.press"]["inputSchema"]["required"] == [
        "page_id",
        "key",
    ]
    assert tools["page.accept_dialog"]["inputSchema"]["required"] == ["page_id"]
    assert tools["page.dismiss_dialog"]["inputSchema"]["required"] == ["page_id"]
    assert tools["context.set_cookies"]["inputSchema"]["required"] == ["cookies"]
    assert tools["context.downloads"]["inputSchema"]["type"] == "object"
    assert tools["context.permission"]["inputSchema"]["required"] == [
        "url",
        "permission",
    ]
    assert tools["context.set_permission"]["inputSchema"]["required"] == [
        "url",
        "permission",
        "setting",
    ]
    assert tools["download.cancel"]["inputSchema"]["required"] == ["guid"]
    assert tools["download.wait"]["inputSchema"]["required"] == ["guid"]
    call = request("tools/call", {"name": "browser.version", "arguments": {}, **meta})
    status, result = post("tools/call", call)
    assert status == 200 and result["result"]["structuredContent"]["version"]

    def call_tool(name, arguments):
        message = request(
            "tools/call",
            {"name": name, "arguments": arguments, **meta},
        )
        status, response = post("tools/call", message)
        assert status == 200 and not response["result"].get("isError"), response
        return response["result"]["structuredContent"]

    return post, request, meta, call_tool
