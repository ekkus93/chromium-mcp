// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_H_

#include <map>
#include <memory>
#include <set>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/agent/browser_agent_protocol.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents.h"
#include "net/server/http_server.h"
#include "net/socket/tcp_server_socket.h"

class Profile;

namespace browser_agent {

class BrowserAgentMcpServerState;
class BrowserAgentConsoleObserver;

// In-process MCP Streamable HTTP endpoint, bound to IPv4 loopback.
class BrowserAgentMcpServer : public net::HttpServer::Delegate,
                              public ProfileObserver {
 public:
  BrowserAgentMcpServer(Profile* profile,
                        std::string access_token,
                        base::FilePath audit_log_path,
                        base::FilePath trace_directory);
  BrowserAgentMcpServer(const BrowserAgentMcpServer&) = delete;
  BrowserAgentMcpServer& operator=(const BrowserAgentMcpServer&) = delete;
  ~BrowserAgentMcpServer() override;

  bool Start(int port);

  void OnProfileWillBeDestroyed(Profile* profile) override;

 private:
  friend class BrowserAgentService;

  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

  void SendJson(int connection_id,
                net::HttpStatusCode status,
                const std::string& body);
  std::string HandleJsonRpc(const base::DictValue& request);
  void StartOnIoThread(int port, base::WaitableEvent* started);
  void StopOnIoThread();
  void HandleRequestOnUiThread(base::DictValue request,
                               int connection_id,
                               base::OnceCallback<void(std::string)> reply);
  bool HandleContextNetworkAndTracingTools(
      const base::DictValue& request,
      int connection_id,
      base::OnceCallback<void(std::string)>& reply);
  bool HandlePageSetupTools(const base::DictValue& request,
                            int connection_id,
                            base::OnceCallback<void(std::string)>& reply);
  bool HandlePageReadTools(const base::DictValue& request,
                           int connection_id,
                           base::OnceCallback<void(std::string)>& reply);
  bool HandlePageEditTools(const base::DictValue& request,
                           int connection_id,
                           base::OnceCallback<void(std::string)>& reply);
  bool HandleContextStorageTools(const base::DictValue& request,
                                 int connection_id,
                                 base::OnceCallback<void(std::string)>& reply);
  bool HandleWaitAndDownloadTools(const base::DictValue& request,
                                  int connection_id,
                                  base::OnceCallback<void(std::string)>& reply);
  void CompleteRequestOnUiThread(
      int connection_id,
      std::shared_ptr<protocol::CancellationSource> cancellation,
      base::OnceCallback<void(std::string)> reply,
      std::string response);
  void CancelRequestOnUiThread(int connection_id);
  void StartEventStreamOnUiThread(int connection_id);
  void StopEventStreamOnUiThread(int connection_id);
  void MaybeDestroyOwnedContext(std::string context_id,
                                base::WeakPtr<Profile> profile);
  void ForgetDestroyedContextProfile(Profile* profile);
  bool HandleBrowserContextToolCall(const base::Value* id,
                                    const protocol::Request& tool_request,
                                    base::DictValue& tool_result,
                                    base::DictValue& result,
                                    std::string& result_text,
                                    std::string* early_response);
  std::string HandleToolCall(const base::Value* id,
                             const base::DictValue& request);
  void PollBrowserEvents();
  void PublishBrowserEvent(base::DictValue event);
  void PublishNavigationEvent(std::string page_id,
                              std::string event_type,
                              std::string url);
  base::RepeatingCallback<void(base::DictValue)> CreateNetworkEventCallback(
      content::WebContents* contents);
  void RecordNetworkEventOnUiThread(std::string page_id, base::DictValue event);
  struct BrowserPageSnapshot {
    std::string url;
    bool is_popup = false;
  };
  std::map<std::string, BrowserPageSnapshot> CollectBrowserPageSnapshot();
  std::string RegisterContext(Profile* profile);
  Profile* ResolveContext(const std::string& context_id);

  const raw_ptr<Profile> profile_;
  const std::string default_context_id_;
  const std::string access_token_;
  const base::FilePath audit_log_path_;
  const base::FilePath trace_directory_;
  scoped_refptr<base::SequencedTaskRunner> audit_task_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;
  scoped_refptr<BrowserAgentMcpServerState> server_state_;
  base::WeakPtr<BrowserAgentMcpServer> weak_this_;
  base::FilePath access_token_path_;
  base::FilePath endpoint_path_;
  base::Thread io_thread_{"BrowserAgentMcpIO"};
  std::unique_ptr<net::HttpServer> http_server_;
  std::map<std::string, tabs::TabHandle> page_handles_;
  std::map<std::string, std::unique_ptr<BrowserAgentConsoleObserver>>
      console_observers_;
  struct PendingRequest {
    std::shared_ptr<protocol::CancellationSource> cancellation;
    base::OnceClosure stop_waiting;
  };
  std::map<int, PendingRequest> pending_requests_;
  std::set<int> event_stream_watches_;
  base::ListValue browser_events_;
  uint64_t next_browser_event_cursor_ = 1;
  std::map<std::string, BrowserPageSnapshot> event_page_snapshot_;
  base::RepeatingTimer event_poll_timer_;
  std::map<std::string, std::string> page_context_ids_;
  std::map<std::string,
           std::pair<std::string, content::GlobalRenderFrameHostId>>
      frame_handles_;
  std::map<std::string, raw_ptr<Profile>> context_handles_;
  std::set<std::string> owned_context_ids_;
  std::map<std::string, base::TimeTicks> pending_context_destructions_;
  // Identity only; a profile can remain listed briefly after its destruction
  // notification. Never dereference an entry in this set.
  std::set<Profile*> destroying_context_profiles_;
  std::map<std::string, std::string> context_accept_languages_;
  std::set<raw_ptr<Profile>> observed_context_profiles_;
  int port_ = -1;
  bool start_succeeded_ = false;
  base::WeakPtrFactory<BrowserAgentMcpServer> weak_factory_{this};
};

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_H_
