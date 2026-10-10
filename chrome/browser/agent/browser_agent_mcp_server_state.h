// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_STATE_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_STATE_H_

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "net/server/http_server.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace browser_agent {

class BrowserAgentMcpServerState
    : public base::RefCountedThreadSafe<BrowserAgentMcpServerState> {
 public:
  BrowserAgentMcpServerState();

  static constexpr size_t kMaximumSubscriptionBytes = 256 * 1024;

  void SetServer(net::HttpServer* server);
  void Send(int connection_id, net::HttpStatusCode status, std::string body);
  void OpenSubscription(int connection_id,
                        std::string subscription_id,
                        bool browser_events);
  bool CloseSubscription(int connection_id);
  void SendResourceUpdated(std::string_view uri);

 private:
  struct Subscription {
    std::string id;
    bool browser_events;
    size_t bytes_sent;
  };

  bool SendEvent(int connection_id, std::string_view notification);
  bool SendSubscriptionMessage(int connection_id,
                               std::string_view method,
                               base::DictValue params,
                               std::string_view subscription_id);

  friend class base::RefCountedThreadSafe<BrowserAgentMcpServerState>;
  ~BrowserAgentMcpServerState();

  raw_ptr<net::HttpServer> server_ = nullptr;
  std::map<int, Subscription> subscriptions_;
};

const net::NetworkTrafficAnnotationTag& GetBrowserAgentMcpTrafficAnnotation();

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_STATE_H_
