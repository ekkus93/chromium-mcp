// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_mcp_server.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/barrier_callback.h"
#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_config.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/agent/browser_agent_console_observer.h"
#include "chrome/browser/agent/browser_agent_protocol.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/javascript_dialogs/app_modal_dialog_controller.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "components/javascript_dialogs/tab_modal_dialog_manager.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_automation.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/dom_storage_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_usage_info.h"
#include "content/public/browser/tracing_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/base/base_window.h"
#include "ui/base/page_transition_types.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

namespace browser_agent {

namespace {
const net::NetworkTrafficAnnotationTag& GetTrafficAnnotation() {
  static const net::NetworkTrafficAnnotationTag annotation =
      net::DefineNetworkTrafficAnnotation("chromium_embedded_mcp", R"(
        semantics {
          sender: "Chromium Embedded MCP Server"
          description: "Serves local MCP requests to the Chromium browser."
          trigger: "A user-enabled local MCP client sends a request."
          data: "Browser state and tool arguments selected by the local client."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "The service requires an explicit feature and command-line opt-in."
          policy_exception_justification: "This is a loopback-only developer feature."
        })");
  return annotation;
}

}  // namespace

class BrowserAgentMcpServerState
    : public base::RefCountedThreadSafe<BrowserAgentMcpServerState> {
 public:
  void SetServer(net::HttpServer* server) {
    server_ = server;
    if (!server_) {
      event_streams_.clear();
    }
  }

  void Send(int connection_id, net::HttpStatusCode status, std::string body) {
    if (server_) {
      server_->Send(connection_id, status, body, "application/json",
                    GetTrafficAnnotation());
    }
  }

  void OpenEventStream(int connection_id) {
    if (!server_) {
      return;
    }
    event_streams_.insert(connection_id);
    SendEvent(
        connection_id,
        R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","logger":"chromium","data":{"event_type":"event_stream_opened"}}})");
  }

  bool CloseEventStream(int connection_id) {
    return event_streams_.erase(connection_id) != 0;
  }

  void SendNotification(const std::string& notification) {
    for (int connection_id : event_streams_) {
      SendEvent(connection_id, notification);
    }
  }

 private:
  void SendEvent(int connection_id, std::string_view notification) {
    const std::string payload =
        base::StrCat({"event: message\ndata: ", notification, "\n\n"});
    const std::string chunk =
        base::StringPrintf("%zx\r\n%s\r\n", payload.size(), payload.c_str());
    server_->SendRaw(connection_id, chunk, GetTrafficAnnotation());
  }

  friend class base::RefCountedThreadSafe<BrowserAgentMcpServerState>;
  ~BrowserAgentMcpServerState() = default;

  raw_ptr<net::HttpServer> server_ = nullptr;
  std::set<int> event_streams_;
};

namespace {

constexpr char kProtocolVersionHeader[] = "mcp-protocol-version";
constexpr char kMethodHeader[] = "mcp-method";
constexpr char kNameHeader[] = "mcp-name";
constexpr char kAuthorizationHeader[] = "authorization";
constexpr char kOriginHeader[] = "origin";
constexpr char kHostHeader[] = "host";
constexpr char kEndpointPath[] = "/mcp";
constexpr size_t kMaximumBodySize = 1024 * 1024;

std::string MakeJsonRpcError(base::Value request_id,
                             int code,
                             std::string message) {
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  base::DictValue error;
  error.Set("code", code);
  error.Set("message", std::move(message));
  response.Set("error", std::move(error));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

}  // namespace

BrowserAgentMcpServer::BrowserAgentMcpServer(Profile* profile,
                                             std::string access_token,
                                             base::FilePath audit_log_path,
                                             base::FilePath trace_directory)
    : profile_(profile),
      default_context_id_(base::UnguessableToken::Create().ToString()),
      access_token_(std::move(access_token)),
      audit_log_path_(std::move(audit_log_path)),
      trace_directory_(std::move(trace_directory)),
      ui_task_runner_(content::GetUIThreadTaskRunner({})),
      server_state_(base::MakeRefCounted<BrowserAgentMcpServerState>()) {
  weak_this_ = weak_factory_.GetWeakPtr();
  if (!audit_log_path_.empty()) {
    audit_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT});
  }
  context_handles_.emplace(default_context_id_, profile);
}

BrowserAgentMcpServer::~BrowserAgentMcpServer() {
  if (io_thread_.IsRunning()) {
    io_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::StopOnIoThread,
                                  base::Unretained(this)));
    io_thread_.Stop();
  }
  if (!access_token_path_.empty()) {
    base::DeleteFile(access_token_path_);
  }
  if (!endpoint_path_.empty()) {
    base::DeleteFile(endpoint_path_);
  }
  for (Profile* context_profile : observed_context_profiles_) {
    context_profile->RemoveObserver(this);
  }
}

void BrowserAgentMcpServer::OnProfileWillBeDestroyed(Profile* profile) {
  profile->RemoveObserver(this);
  std::erase_if(context_handles_, [profile](const auto& entry) {
    return entry.second == profile;
  });
  observed_context_profiles_.erase(profile);
}

bool BrowserAgentMcpServer::Start(int port) {
  const base::FilePath runtime_directory =
      profile_->GetPath().AppendASCII("BrowserAgentRuntime");
  if (!base::CreateDirectory(runtime_directory)) {
    return false;
  }
  if (!trace_directory_.empty() && !base::CreateDirectory(trace_directory_)) {
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(runtime_directory, 0700)) {
    return false;
  }
#endif
  access_token_path_ = runtime_directory.AppendASCII("browser-agent-mcp-token");
  if (!base::WriteFile(access_token_path_, access_token_)) {
    access_token_path_.clear();
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(access_token_path_, 0600)) {
    base::DeleteFile(access_token_path_);
    access_token_path_.clear();
    return false;
  }
#endif

  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!io_thread_.StartWithOptions(std::move(options))) {
    return false;
  }
  io_task_runner_ = io_thread_.task_runner();
  base::WaitableEvent started;
  io_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::StartOnIoThread,
                                base::Unretained(this), port, &started));
  started.Wait();
  if (!start_succeeded_) {
    return false;
  }
  endpoint_path_ = runtime_directory.AppendASCII("browser-agent-mcp-endpoint");
  const std::string endpoint = base::StrCat(
      {"http://127.0.0.1:", base::NumberToString(port_), kEndpointPath, "\n"});
  if (!base::WriteFile(endpoint_path_, endpoint)) {
    endpoint_path_.clear();
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(endpoint_path_, 0600)) {
    base::DeleteFile(endpoint_path_);
    endpoint_path_.clear();
    return false;
  }
#endif
  LOG(INFO) << "event=browser_agent_mcp_started endpoint=http://127.0.0.1:"
            << port_ << kEndpointPath
            << " token_file=" << access_token_path_.AsUTF8Unsafe();
  return true;
}

void BrowserAgentMcpServer::StartOnIoThread(int port,
                                            base::WaitableEvent* started) {
  auto server_socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  net::IPEndPoint address(net::IPAddress::IPv4Localhost(),
                          static_cast<uint16_t>(port));
  if (server_socket->Listen(address, 8, std::nullopt) == net::OK) {
    net::IPEndPoint bound_address;
    if (server_socket->GetLocalAddress(&bound_address) == net::OK) {
      port_ = bound_address.port();
      http_server_ =
          std::make_unique<net::HttpServer>(std::move(server_socket), this);
      server_state_->SetServer(http_server_.get());
      start_succeeded_ = true;
    }
  }
  started->Signal();
}

void BrowserAgentMcpServer::StopOnIoThread() {
  server_state_->SetServer(nullptr);
  http_server_.reset();
}

void BrowserAgentMcpServer::OnConnect(int connection_id) {}

void BrowserAgentMcpServer::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  if (info.path != kEndpointPath || !info.peer.address().IsLoopback() ||
      info.GetHeaderValue(kOriginHeader).size() ||
      info.GetHeaderValue(kHostHeader) !=
          base::StrCat({"127.0.0.1:", base::NumberToString(port_)})) {
    SendJson(connection_id, net::HTTP_FORBIDDEN,
             R"({"error":"local MCP endpoint rejected this request"})");
    return;
  }
  const std::string authorization = info.GetHeaderValue(kAuthorizationHeader);
  constexpr std::string_view kBearerPrefix = "Bearer ";
  if (!authorization.starts_with(kBearerPrefix) ||
      !protocol::IsValidAccessToken(
          access_token_,
          std::string_view(authorization).substr(kBearerPrefix.size()))) {
    SendJson(connection_id, net::HTTP_UNAUTHORIZED,
             R"({"error":"MCP authentication required"})");
    return;
  }
  if (info.method == "GET") {
    const std::string accept =
        base::ToLowerASCII(info.GetHeaderValue("accept"));
    if (accept.find("text/event-stream") == std::string::npos) {
      SendJson(
          connection_id, net::HTTP_NOT_ACCEPTABLE,
          R"({"error":"event stream requires Accept: text/event-stream"})");
      return;
    }
    net::HttpServerResponseInfo response(net::HTTP_OK);
    response.AddHeader("Content-Type", "text/event-stream");
    response.AddHeader("Cache-Control", "no-cache");
    response.AddHeader("Connection", "keep-alive");
    response.AddHeader("Transfer-Encoding", "chunked");
    http_server_->SendResponse(connection_id, response, GetTrafficAnnotation());
    server_state_->OpenEventStream(connection_id);
    ui_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&BrowserAgentMcpServer::StartEventStreamOnUiThread,
                       weak_this_, connection_id));
    return;
  }
  if (info.method != "POST") {
    SendJson(
        connection_id, net::HTTP_METHOD_NOT_ALLOWED,
        R"({"error":"MCP requests must use POST or an event-stream GET"})");
    return;
  }

  if (info.data.size() > kMaximumBodySize) {
    SendJson(connection_id, net::HTTP_REQUEST_ENTITY_TOO_LARGE,
             R"({"error":"request body too large"})");
    return;
  }

  const std::string protocol_version =
      info.GetHeaderValue(kProtocolVersionHeader);
  std::optional<base::Value> value = base::JSONReader::Read(info.data, 0);
  if (!value) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             MakeJsonRpcError(base::Value(), -32700, "Parse error."));
    return;
  }
  if (!value->is_dict()) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             MakeJsonRpcError(base::Value(), -32600, "Invalid request."));
    return;
  }
  const base::DictValue& request = value->GetDict();
  const std::string* method = request.FindString("method");
  if (!method ||
      info.GetHeaderValue(kProtocolVersionHeader) !=
          protocol::kMcpProtocolVersion ||
      info.GetHeaderValue(kMethodHeader) != *method) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             R"({"error":"MCP protocol headers do not match the request"})");
    return;
  }
  const base::DictValue* params = request.FindDict("params");
  const base::DictValue* metadata =
      params ? params->FindDict("_meta") : nullptr;
  if (metadata) {
    const std::string* request_protocol_version =
        metadata->FindString("io.modelcontextprotocol/protocolVersion");
    if (request_protocol_version &&
        *request_protocol_version != protocol_version) {
      SendJson(
          connection_id, net::HTTP_BAD_REQUEST,
          R"({"error":"MCP protocol version header does not match metadata"})");
      return;
    }
  }
  if (*method == "tools/call") {
    const base::DictValue* call_params = request.FindDict("params");
    const std::string* tool_name =
        call_params ? call_params->FindString("name") : nullptr;
    if (!tool_name || info.GetHeaderValue(kNameHeader) != *tool_name) {
      SendJson(connection_id, net::HTTP_BAD_REQUEST,
               R"({"error":"Mcp-Name does not match the tool call"})");
      return;
    }
  }
  if (*method == "resources/read") {
    const base::DictValue* read_params = request.FindDict("params");
    const std::string* resource_uri =
        read_params ? read_params->FindString("uri") : nullptr;
    if (!resource_uri || info.GetHeaderValue(kNameHeader) != *resource_uri) {
      SendJson(connection_id, net::HTTP_BAD_REQUEST,
               R"({"error":"Mcp-Name does not match the resource URI"})");
      return;
    }
  }
  if (audit_task_runner_) {
    base::DictValue event;
    event.Set(
        "time_unix_ms",
        static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
    event.Set("event", "mcp_request");
    event.Set("method", *method);
    if (*method == "tools/call") {
      event.Set("tool", *request.FindDict("params")->FindString("name"));
    }
    std::string line =
        base::WriteJson(base::Value(std::move(event))).value_or("{}");
    line.push_back('\n');
    audit_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::FilePath path, std::string line) {
                         base::File file(path, base::File::FLAG_OPEN_ALWAYS |
                                                   base::File::FLAG_APPEND);
                         if (file.IsValid()) {
                           file.WriteAtCurrentPos(base::as_byte_span(line));
                         }
                       },
                       audit_log_path_, std::move(line)));
  }
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &BrowserAgentMcpServer::HandleRequestOnUiThread, weak_this_,
          request.Clone(), connection_id,
          base::BindPostTask(
              io_task_runner_,
              base::BindOnce(&BrowserAgentMcpServerState::Send, server_state_,
                             connection_id, net::HTTP_OK))));
}

void BrowserAgentMcpServer::PublishBrowserEvent(base::DictValue event) {
  base::DictValue params;
  params.Set("level", "info");
  params.Set("logger", "chromium.browser");
  params.Set("data", std::move(event));
  base::DictValue notification;
  notification.Set("jsonrpc", "2.0");
  notification.Set("method", "notifications/message");
  notification.Set("params", std::move(params));
  std::string serialized =
      base::WriteJson(base::Value(std::move(notification))).value_or("{}");
  io_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<BrowserAgentMcpServerState> server_state,
             std::string message) { server_state->SendNotification(message); },
          server_state_, std::move(serialized)));
}

void BrowserAgentMcpServer::PublishNavigationEvent(std::string page_id,
                                                   std::string event_type,
                                                   std::string url) {
  base::DictValue event;
  event.Set("event_type", std::move(event_type));
  event.Set("page_id", std::move(page_id));
  event.Set("url", std::move(url));
  PublishBrowserEvent(std::move(event));
}

base::RepeatingCallback<void(base::DictValue)>
BrowserAgentMcpServer::CreateNetworkEventCallback(
    content::WebContents* contents) {
  if (!contents) {
    return {};
  }
  for (const auto& [page_id, handle] : page_handles_) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && tab->GetContents() == contents) {
      return base::BindPostTask(
          ui_task_runner_,
          base::BindRepeating(
              &BrowserAgentMcpServer::RecordNetworkEventOnUiThread,
              weak_factory_.GetWeakPtr(), page_id));
    }
  }
  return {};
}

void BrowserAgentMcpServer::RecordNetworkEventOnUiThread(
    std::string page_id,
    base::DictValue event) {
  auto handle = page_handles_.find(page_id);
  if (handle == page_handles_.end()) {
    return;
  }
  tabs::TabInterface* tab = handle->second.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return;
  }
  auto& observer = console_observers_[page_id];
  if (!observer) {
    observer =
        std::make_unique<BrowserAgentConsoleObserver>(contents, std::string());
  }
  event.Set("page_id", page_id);
  observer->AddNetworkEvent(event.Clone());
  PublishBrowserEvent(std::move(event));
}

std::string BrowserAgentMcpServer::RegisterContext(Profile* profile) {
  for (const auto& [context_id, registered_profile] : context_handles_) {
    if (registered_profile == profile) {
      return context_id;
    }
  }
  if (profile != profile_ &&
      observed_context_profiles_.insert(profile).second) {
    profile->AddObserver(this);
  }
  const std::string context_id = base::UnguessableToken::Create().ToString();
  context_handles_.emplace(context_id, profile);
  return context_id;
}

Profile* BrowserAgentMcpServer::ResolveContext(const std::string& context_id) {
  auto it = context_handles_.find(context_id);
  if (it == context_handles_.end()) {
    return nullptr;
  }
  Profile* target = it->second;
  if (target == profile_) {
    return profile_;
  }
  for (Profile* live_profile : profile_->GetAllOffTheRecordProfiles()) {
    if (live_profile == target) {
      return target;
    }
  }
  context_handles_.erase(it);
  return nullptr;
}

void BrowserAgentMcpServer::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  SendJson(connection_id, net::HTTP_BAD_REQUEST,
           R"({"error":"WebSocket transport is not supported"})");
}

void BrowserAgentMcpServer::OnWebSocketMessage(int connection_id,
                                               std::string data) {
  http_server_->Close(connection_id);
}

void BrowserAgentMcpServer::OnClose(int connection_id) {
  if (server_state_->CloseEventStream(connection_id)) {
    ui_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&BrowserAgentMcpServer::StopEventStreamOnUiThread,
                       weak_this_, connection_id));
  }
  ui_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::CancelRequestOnUiThread,
                                weak_this_, connection_id));
}

void BrowserAgentMcpServer::SendJson(int connection_id,
                                     net::HttpStatusCode status,
                                     const std::string& body) {
  server_state_->Send(connection_id, status, body);
}

}  // namespace browser_agent
