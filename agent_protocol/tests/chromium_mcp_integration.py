#!/usr/bin/env python3
"""Exercise the opt-in embedded MCP endpoint in a built Chromium binary."""

from dataclasses import dataclass, field
import json
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.error
from urllib.parse import urlsplit
import urllib.request

from chromium_mcp_integration_pages import (
    run_final_scenarios,
    run_page_interaction_scenarios,
    run_page_read_scenarios,
)
from chromium_mcp_integration_storage import run_storage_download_scenarios
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
        if self.path == "/large":
            body = b"L" * (1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except BrokenPipeError:
                pass
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/redirected")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/redirected":
            body = b"redirect target body"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/continuous":
            chunk = b"C" * (64 * 1024)
            self.send_response(200)
            self.send_header("Content-Length", str(32 * 1024 * 1024))
            self.end_headers()
            try:
                for _ in range(512):
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    time.sleep(0.01)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path == "/disabled":
            body = (
                b"<!doctype html><body>pending<script>fetch('/large')"
                b".then(r=>r.arrayBuffer()).then(b=>{document.body.textContent="
                b"String(b.byteLength)})</script></body>"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
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
        body = b"<!doctype html><title>Slow fixture</title><h1>Fixture heading</h1><a href='/next'>Next</a><a id='popup' href='/next' onclick=\"window.open('/next','fixture-popup','popup,width=300,height=250');return false\">Open popup</a><button aria-description='Click action' aria-expanded='false' onmouseenter='window.hovered=event.isTrusted' onclick='window.clicked=event.isTrusted'>Click</button><button id='prompt' onclick='window.promptResult=prompt(\"Name?\",\"default\")'>Prompt</button><label for='typed'>Typed label</label><input id='typed' onkeydown='window.lastKey=event.key'><input id='readonly' readonly value='fixed'><span id='referenced-name'>Reference name</span><button id='referenced' aria-labelledby='referenced-name'>ignored button text</button><button aria-label='ARIA label name'>ignored button text</button><img id='alternative' alt='Alternative image' style='width:40px;height:20px'><button id='covered' style='position:absolute;left:150px;top:400px;width:100px;height:40px'>Covered button</button><div id='cover' style='position:absolute;left:150px;top:400px;width:100px;height:40px;z-index:5'></div><button id='hidden' style='display:none'>Hidden</button><button id='offscreen' style='position:absolute;top:200vh'>Offscreen</button><input id='agree' type='checkbox' onchange='window.checkboxTrusted=event.isTrusted'><input id='upload' type='file'><select id='choice' onchange='window.selected=this.value'><option value='one'>One</option><option value='two'>Two</option></select><iframe sandbox='allow-scripts' srcdoc='&lt;!doctype html&gt;&lt;html&gt;&lt;head&gt;&lt;title&gt;Cross origin&lt;/title&gt;&lt;/head&gt;&lt;body&gt;child frame&lt;/body&gt;&lt;/html&gt;'></iframe>ready"
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
        token_path = (
            profile / "profile/Default/BrowserAgentRuntime/browser-agent-mcp-token"
        )
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
        assert (
            token_path.exists() and endpoint_path.exists()
        ), f"MCP credentials were not created; see {log_path}"
        token = token_path.read_text().strip()
        endpoint = endpoint_path.read_text().strip()
        sdk_pythonpath = os.environ.get("CHROMIUM_MCP_SDK_PYTHONPATH")
        if sdk_pythonpath:
            sdk_environment = os.environ.copy()
            sdk_environment["PYTHONPATH"] = os.pathsep.join(
                filter(
                    None,
                    [sdk_pythonpath, sdk_environment.get("PYTHONPATH", "")],
                )
            )
            subprocess.run(
                [
                    "python3",
                    str(Path(__file__).with_name("mcp_sdk_interop.py")),
                    endpoint,
                    token,
                ],
                check=True,
                env=sdk_environment,
                timeout=60,
            )

        from chromium_mcp_integration_client import create_mcp_client

        post, request, meta, call_tool = create_mcp_client(endpoint, token)

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
        shutdown_target = urlsplit(endpoint)
        shutdown_connection = HTTPConnection(
            shutdown_target.hostname, shutdown_target.port, timeout=10
        )
        shutdown_connection.request(
            "POST",
            shutdown_target.path,
            body=json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": "shutdown-listener",
                    "method": "subscriptions/listen",
                    "params": {
                        "notifications": {
                            "resourceSubscriptions": ["chromium-mcp://browser/events"]
                        },
                        **meta,
                    },
                }
            ),
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
                "MCP-Protocol-Version": PROTOCOL_VERSION,
                "Mcp-Method": "subscriptions/listen",
                "Authorization": f"Bearer {token}",
                "Host": shutdown_target.netloc,
            },
        )
        shutdown_response = shutdown_connection.getresponse()
        assert shutdown_response.status == 200
        acknowledgement = b""
        while not acknowledgement.startswith(b"data: "):
            acknowledgement = shutdown_response.readline()
            assert acknowledgement, "shutdown listener closed before acknowledgment"
        assert json.loads(acknowledgement[6:])["method"] == (
            "notifications/subscriptions/acknowledged"
        )
        process.terminate()
        process.wait(timeout=8)
        shutdown_tail = bytearray()
        while True:
            line = shutdown_response.readline()
            if not line:
                break
            shutdown_tail.extend(line)
        assert b"notifications/subscriptions/acknowledged" not in shutdown_tail
        shutdown_connection.close()
        incognito_profile = profile / "preexisting-incognito-profile"
        incognito_log_path = profile / "incognito-chrome.log"
        with incognito_log_path.open("w") as incognito_log:
            incognito_process = subprocess.Popen(
                [
                    CHROME,
                    "--headless=new",
                    "--no-sandbox",
                    "--disable-gpu",
                    "--disable-dev-shm-usage",
                    "--no-first-run",
                    "--incognito",
                    f"--user-data-dir={incognito_profile}",
                    "--enable-features=BrowserAgentService",
                    "--enable-browser-agent-service",
                    "--browser-agent-mcp-port=0",
                    "about:blank",
                ],
                stdout=incognito_log,
                stderr=subprocess.STDOUT,
            )
            try:
                runtime = incognito_profile / "Default/BrowserAgentRuntime"
                incognito_token_path = runtime / "browser-agent-mcp-token"
                incognito_endpoint_path = runtime / "browser-agent-mcp-endpoint"
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and not (
                    incognito_token_path.exists() and incognito_endpoint_path.exists()
                ):
                    if incognito_process.poll() is not None:
                        raise AssertionError(
                            f"Incognito Chromium exited; see {incognito_log_path}"
                        )
                    time.sleep(0.1)
                assert incognito_token_path.exists()
                incognito_token = incognito_token_path.read_text().strip()
                incognito_endpoint = incognito_endpoint_path.read_text().strip()

                def incognito_post(method, params):
                    message = {
                        "jsonrpc": "2.0",
                        "id": "preexisting-incognito",
                        "method": method,
                        "params": {
                            **params,
                            "_meta": {
                                "io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION,
                                "io.modelcontextprotocol/clientCapabilities": {},
                            },
                        },
                    }
                    headers = {
                        "Authorization": f"Bearer {incognito_token}",
                        "Content-Type": "application/json",
                        "MCP-Protocol-Version": PROTOCOL_VERSION,
                        "Mcp-Method": method,
                    }
                    if method == "tools/call":
                        headers["Mcp-Name"] = params["name"]
                    request = urllib.request.Request(
                        incognito_endpoint,
                        data=json.dumps(message).encode(),
                        headers=headers,
                        method="POST",
                    )
                    with urllib.request.urlopen(request, timeout=10) as response:
                        return json.loads(response.read())

                incognito_contexts = incognito_post(
                    "tools/call",
                    {"name": "browser.list_contexts", "arguments": {}},
                )["result"]["structuredContent"]["contexts"]
                preexisting = next(
                    item for item in incognito_contexts if item["off_the_record"]
                )
                close_result = incognito_post(
                    "tools/call",
                    {
                        "name": "browser.close_context",
                        "arguments": {"context_id": preexisting["context_id"]},
                    },
                )["result"]
                assert close_result.get("isError"), close_result
                still_open = incognito_post(
                    "tools/call",
                    {"name": "browser.list_contexts", "arguments": {}},
                )["result"]["structuredContent"]["contexts"]
                assert any(
                    item["context_id"] == preexisting["context_id"]
                    for item in still_open
                ), still_open
            finally:
                incognito_process.terminate()
                try:
                    incognito_process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    incognito_process.kill()
                    incognito_process.wait()
        disabled_profile = profile / "disabled-profile"
        disabled = subprocess.run(
            [
                CHROME,
                "--headless=new",
                "--no-sandbox",
                "--disable-gpu",
                "--disable-dev-shm-usage",
                "--no-first-run",
                "--virtual-time-budget=5000",
                f"--user-data-dir={disabled_profile}",
                "--dump-dom",
                f"http://127.0.0.1:{fixture_server.server_port}/disabled",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert "1048576" in disabled.stdout, disabled.stdout[-2000:]
        assert not (
            disabled_profile / "Default/BrowserAgentRuntime/browser-agent-mcp-endpoint"
        ).exists()
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
