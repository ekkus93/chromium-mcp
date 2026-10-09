#!/usr/bin/env python3
"""Exercise the opt-in embedded MCP endpoint in a built Chromium binary."""

from dataclasses import dataclass, field
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

from chromium_mcp_integration_pages import (
    run_final_scenarios,
    run_page_interaction_scenarios,
    run_page_read_scenarios,
    run_storage_download_scenarios,
)
from chromium_mcp_integration_setup import (
    run_context_scenarios,
    run_page_setup_scenarios,
)


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
        time.sleep(0.4)
        # Use a sandboxed srcdoc frame to test cross-origin automation without
        # depending on a second HTTP navigation completing in headless Chrome.
        body = (
            b"<!doctype html><title>Slow fixture</title><h1>Fixture heading</h1><a href='/next'>Next</a><a id='popup' href='/next' onclick=\"window.open('/next','fixture-popup','popup,width=300,height=250');return false\">Open popup</a><button aria-description='Click action' aria-expanded='false' onmouseenter='window.hovered=event.isTrusted' onclick='window.clicked=event.isTrusted'>Click</button><button id='prompt' onclick='window.promptResult=prompt(\"Name?\",\"default\")'>Prompt</button><input id='typed' onkeydown='window.lastKey=event.key'><input id='agree' type='checkbox' onchange='window.checkboxTrusted=event.isTrusted'><input id='upload' type='file'><select id='choice' onchange='window.selected=this.value'><option value='one'>One</option><option value='two'>Two</option></select><iframe sandbox='allow-scripts' srcdoc='&lt;!doctype html&gt;&lt;html&gt;&lt;head&gt;&lt;title&gt;Cross origin&lt;/title&gt;&lt;/head&gt;&lt;body&gt;child frame&lt;/body&gt;&lt;/html&gt;'></iframe>ready"
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


@dataclass
class IntegrationState:
    post: object
    request: object
    call_tool: object
    meta: dict
    endpoint: str
    token: str
    fixture_server: ThreadingHTTPServer
    profile: Path
    trace_directory: Path
    upload_path: Path
    audit_path: Path
    protocol_version: str
    accept_language_headers: list
    cookie_url: str = ""
    page_id: str = ""
    target_url: str = ""
    permission_to_check: str = ""
    button_bounds: dict = field(default_factory=dict)


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

        state = IntegrationState(
            post=post,
            request=request,
            call_tool=call_tool,
            meta=meta,
            endpoint=endpoint,
            token=token,
            fixture_server=fixture_server,
            profile=profile,
            trace_directory=trace_directory,
            upload_path=upload_path,
            audit_path=audit_path,
            protocol_version=PROTOCOL_VERSION,
            accept_language_headers=ACCEPT_LANGUAGE_HEADERS,
        )
        run_context_scenarios(state)
        run_page_setup_scenarios(state)
        run_storage_download_scenarios(state)
        run_page_read_scenarios(state)
        run_page_interaction_scenarios(state)
        run_final_scenarios(state)
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
