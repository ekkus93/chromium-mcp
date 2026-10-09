#!/usr/bin/env python3
"""Exercise the opt-in embedded MCP endpoint in a built Chromium binary."""

import base64
from http.client import HTTPConnection
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit


CHROME = os.environ.get("CHROME_BIN")
PROTOCOL_VERSION = "2026-07-28"
ACCEPT_LANGUAGE_HEADERS = []


class SlowFixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        ACCEPT_LANGUAGE_HEADERS.append(self.headers.get("Accept-Language", ""))
        if self.path == "/slow_download":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", "attachment; filename=slow.bin")
            self.send_header("Content-Length", str(20 * 1024))
            self.end_headers()
            try:
                for _ in range(20):
                    self.wfile.write(b"x" * 1024)
                    self.wfile.flush()
                    time.sleep(0.1)
            except BrokenPipeError:
                pass
            return
        if self.path == "/download":
            body = b"native download fixture\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Disposition", "attachment; filename=fixture.txt")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/cross":
            body = b"<!doctype html><title>Cross origin</title><body>child frame</body>"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        time.sleep(0.4)
        body = (
            b"<!doctype html><title>Slow fixture</title><h1>Fixture heading</h1><a href='/next'>Next</a><a id='popup' href='/next' onclick=\"window.open('/next','fixture-popup','popup,width=300,height=250');return false\">Open popup</a><button aria-description='Click action' aria-expanded='false' onmouseenter='window.hovered=event.isTrusted' onclick='window.clicked=event.isTrusted'>Click</button><button id='prompt' onclick='window.promptResult=prompt(\"Name?\",\"default\")'>Prompt</button><input id='typed' onkeydown='window.lastKey=event.key'><input id='agree' type='checkbox' onchange='window.checkboxTrusted=event.isTrusted'><input id='upload' type='file'><select id='choice' onchange='window.selected=this.value'><option value='one'>One</option><option value='two'>Two</option></select><iframe src='http://localhost:%d/cross'></iframe>ready"
            % self.server.server_port
        )
        self.send_response(200)
        self.send_header("X-Mcp-Fixture", "navigation-response")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def log_message(self, *_args):
        pass


def main():
    if not CHROME:
        raise SystemExit("Set CHROME_BIN to a built Chromium executable.")
    profile = Path(tempfile.mkdtemp(prefix="chromium-mcp-test-"))
    log_path = profile / "chrome.log"
    audit_path = profile / "audit.jsonl"
    trace_directory = profile / "traces"
    upload_path = profile / "upload.txt"
    upload_path.write_text("native file chooser fixture\n")
    log = log_path.open("w")
    fixture_server = ThreadingHTTPServer(("127.0.0.1", 0), SlowFixtureHandler)
    fixture_thread = threading.Thread(target=fixture_server.serve_forever, daemon=True)
    fixture_thread.start()
    process = subprocess.Popen(
        [
            CHROME,
            "--headless=new",
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--no-first-run",
            f"--user-data-dir={profile / 'profile'}",
            "--enable-features=BrowserAgentService",
            "--disable-features=BackForwardCache,Btm",
            "--enable-browser-agent-service",
            "--browser-agent-mcp-port=0",
            f"--browser-agent-audit-log={audit_path}",
            f"--browser-agent-trace-dir={trace_directory}",
            "about:blank",
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        token_path = profile / "profile/Default/BrowserAgentRuntime/browser-agent-mcp-token"
        endpoint_path = (
            profile / "profile/Default/BrowserAgentRuntime/browser-agent-mcp-endpoint"
        )
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not (
            token_path.exists() and endpoint_path.exists()
        ):
            if process.poll() is not None:
                raise AssertionError(f"Chromium exited; see {log_path}")
            time.sleep(0.1)
        assert token_path.exists() and endpoint_path.exists(), (
            f"MCP credentials were not created; see {log_path}"
        )
        token = token_path.read_text().strip()
        endpoint = endpoint_path.read_text().strip()

        def post(method, message, *, authenticate=True, origin=None, header_method=None):
            headers = {
                "Content-Type": "application/json",
                "MCP-Protocol-Version": PROTOCOL_VERSION,
                "Mcp-Method": header_method or method,
            }
            if authenticate:
                headers["Authorization"] = f"Bearer {token}"
            if method == "tools/call":
                headers["Mcp-Name"] = message["params"]["name"]
            if method == "resources/read":
                headers["Mcp-Name"] = message["params"]["uri"]
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

        meta = {"_meta": {"io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION}}
        request = lambda method, params: {
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params,
        }
        assert post("server/discover", request("server/discover", meta))[0] == 200
        status, resources = post("resources/list", request("resources/list", meta))
        assert status == 200
        resource_uris = {item["uri"] for item in resources["result"]["resources"]}
        assert "chromium-mcp://browser/version" in resource_uris
        assert "chromium-mcp://browser/contexts" in resource_uris
        status, version_resource = post(
            "resources/read",
            request(
                "resources/read",
                {"uri": "chromium-mcp://browser/version", **meta},
            ),
        )
        assert status == 200
        assert json.loads(version_resource["result"]["contents"][0]["text"])[
            "version"
        ]
        status, missing_resource = post(
            "resources/read",
            request("resources/read", {"uri": "chromium-mcp://browser/missing", **meta}),
        )
        assert status == 200 and missing_resource["error"]["code"] == -32602
        status, invalid_version = post(
            "tools/list", {**request("tools/list", meta), "jsonrpc": "1.0"}
        )
        assert status == 200 and invalid_version["error"]["code"] == -32600
        status, missing_id = post(
            "tools/list", {"jsonrpc": "2.0", "method": "tools/list", "params": meta}
        )
        assert status == 200 and missing_id["id"] is None
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
        assert tools["page.wait_for_load_state"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["page.wait_for_url"]["inputSchema"]["required"] == [
            "page_id",
            "url",
        ]
        assert tools["page.frames"]["inputSchema"]["required"] == ["page_id"]
        assert tools["page.find_elements"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert "frame_id" in tools["page.find_elements"]["inputSchema"]["properties"]
        assert tools["page.storage_state"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert "frame_id" in tools["page.storage_state"]["inputSchema"]["properties"]
        assert tools["page.set_storage"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["page.download"]["inputSchema"]["required"] == [
            "page_id",
            "url",
        ]
        assert tools["page.accessibility_snapshot"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["page.visible_content"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["page.console_messages"]["inputSchema"]["required"] == [
            "page_id"
        ]
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
        assert tools["page.accept_dialog"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["page.dismiss_dialog"]["inputSchema"]["required"] == [
            "page_id"
        ]
        assert tools["context.set_cookies"]["inputSchema"]["required"] == [
            "cookies"
        ]
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
                    and frame["url"].endswith("/cross")
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
        cross_frame_storage = call_tool(
            "page.set_storage",
            {
                "page_id": page_id,
                "frame_id": child_frame["frame_id"],
                "local": {"cross_origin_key": "cross_origin_value"},
            },
        )
        assert cross_frame_storage["origin"].startswith("http://localhost:")
        assert cross_frame_storage["local"]["cross_origin_key"] == "cross_origin_value"
        assert call_tool(
            "page.storage_state",
            {"page_id": page_id, "frame_id": child_frame["frame_id"]},
        )["local"]["cross_origin_key"] == "cross_origin_value"
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
    except Exception:
        log.flush()
        print(log_path.read_text(errors="replace")[-4000:])
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        fixture_server.shutdown()
        fixture_server.server_close()
        fixture_thread.join(timeout=2)
        log.close()
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
