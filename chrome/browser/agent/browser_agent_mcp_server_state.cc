// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_mcp_server_state.h"

#include <utility>
#include <vector>

#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace browser_agent {

const net::NetworkTrafficAnnotationTag& GetBrowserAgentMcpTrafficAnnotation() {
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

BrowserAgentMcpServerState::BrowserAgentMcpServerState() = default;

BrowserAgentMcpServerState::~BrowserAgentMcpServerState() = default;

void BrowserAgentMcpServerState::SetServer(net::HttpServer* server) {
  server_ = server;
  if (!server_) {
    subscriptions_.clear();
  }
}

void BrowserAgentMcpServerState::Send(int connection_id,
                                      net::HttpStatusCode status,
                                      std::string body) {
  if (server_) {
    server_->Send(connection_id, status, body, "application/json",
                  GetBrowserAgentMcpTrafficAnnotation());
  }
}

void BrowserAgentMcpServerState::OpenSubscription(int connection_id,
                                                  std::string subscription_id,
                                                  bool browser_events) {
  if (!server_) {
    return;
  }
  subscriptions_[connection_id] =
      Subscription{std::move(subscription_id), browser_events, 0};
  const Subscription& subscription = subscriptions_[connection_id];
  base::DictValue params;
  base::DictValue notifications;
  base::ListValue resources;
  if (browser_events) {
    resources.Append("chromium-mcp://browser/events");
  }
  notifications.Set("resourceSubscriptions", std::move(resources));
  params.Set("notifications", std::move(notifications));
  if (!SendSubscriptionMessage(connection_id,
                               "notifications/subscriptions/acknowledged",
                               std::move(params), subscription.id)) {
    server_->Close(connection_id);
  }
}

bool BrowserAgentMcpServerState::CloseSubscription(int connection_id) {
  return subscriptions_.erase(connection_id) != 0;
}

void BrowserAgentMcpServerState::SendResourceUpdated(std::string_view uri) {
  std::vector<int> exhausted_connections;
  for (const auto& [connection_id, subscription] : subscriptions_) {
    if (!subscription.browser_events) {
      continue;
    }
    base::DictValue params;
    params.Set("uri", uri);
    if (!SendSubscriptionMessage(connection_id,
                                 "notifications/resources/updated",
                                 std::move(params), subscription.id)) {
      exhausted_connections.push_back(connection_id);
    }
  }
  // A closed stream is an explicit recovery signal. The client can reopen the
  // subscription and read the browser-events resource from its cursor.
  for (int connection_id : exhausted_connections) {
    server_->Close(connection_id);
  }
}

bool BrowserAgentMcpServerState::SendEvent(int connection_id,
                                           std::string_view notification) {
  const std::string payload =
      base::StrCat({"event: message\ndata: ", notification, "\n\n"});
  const std::string chunk =
      base::StringPrintf("%zx\r\n%s\r\n", payload.size(), payload.c_str());
  auto subscription = subscriptions_.find(connection_id);
  if (subscription == subscriptions_.end() ||
      chunk.size() >
          kMaximumSubscriptionBytes - subscription->second.bytes_sent) {
    return false;
  }
  subscription->second.bytes_sent += chunk.size();
  server_->SendRaw(connection_id, chunk, GetBrowserAgentMcpTrafficAnnotation());
  return true;
}

bool BrowserAgentMcpServerState::SendSubscriptionMessage(
    int connection_id,
    std::string_view method,
    base::DictValue params,
    std::string_view subscription_id) {
  base::DictValue message;
  message.Set("jsonrpc", "2.0");
  message.Set("method", method);
  base::DictValue meta;
  meta.Set("io.modelcontextprotocol/subscriptionId", subscription_id);
  params.Set("_meta", std::move(meta));
  message.Set("params", std::move(params));
  return SendEvent(
      connection_id,
      base::WriteJson(base::Value(std::move(message))).value_or("{}"));
}

}  // namespace browser_agent
