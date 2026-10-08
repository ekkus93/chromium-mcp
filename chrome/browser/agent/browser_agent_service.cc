// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_service.h"

#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/unguessable_token.h"
#include "chrome/browser/agent/browser_agent_mcp_server.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"

namespace browser_agent {

BrowserAgentService::BrowserAgentService(Profile* profile, Config config)
    : config_(std::move(config)),
      session_capability_("cap_" + base::UnguessableToken::Create().ToString()),
      mcp_port_(config_.mcp_port) {
  CHECK(profile);
  LOG(INFO) << "event=browser_agent_service_started";
  if (mcp_port_ >= 0) {
    mcp_server_ = std::make_unique<BrowserAgentMcpServer>(
        profile, session_capability_, config_.audit_log_path,
        config_.trace_directory);
    if (!mcp_server_->Start(mcp_port_)) {
      LOG(ERROR) << "event=browser_agent_mcp_start_failed";
      mcp_server_.reset();
    }
  }
}

BrowserAgentService::~BrowserAgentService() = default;

void BrowserAgentService::Shutdown() {
  if (is_shutdown_) {
    return;
  }
  is_shutdown_ = true;
  LOG(INFO) << "event=browser_agent_service_stopped";
}

base::RepeatingCallback<void(base::DictValue)>
BrowserAgentService::CreateNetworkEventCallback(
    content::WebContents* contents) {
  return mcp_server_ ? mcp_server_->CreateNetworkEventCallback(contents)
                     : base::RepeatingCallback<void(base::DictValue)>();
}

// static
bool BrowserAgentService::IsExplicitlyEnabled() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  return command_line->HasSwitch(switches::kEnableBrowserAgentService) &&
         base::FeatureList::IsEnabled(features::kBrowserAgentService);
}

// static
bool BrowserAgentService::GetConfigFromCommandLine(Config* config) {
  CHECK(config);
  if (!IsExplicitlyEnabled()) {
    return false;
  }

  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  Config candidate;
  if (command_line->HasSwitch(switches::kBrowserAgentMcpPort)) {
    int port = -1;
    if (!base::StringToInt(
            command_line->GetSwitchValueASCII(switches::kBrowserAgentMcpPort),
            &port) ||
        port < 0 || port > 65535) {
      LOG(ERROR)
          << "event=browser_agent_service_disabled reason=invalid_mcp_port";
      return false;
    }
    candidate.mcp_port = port;
  }
  if (command_line->HasSwitch(switches::kBrowserAgentAuditLog)) {
    candidate.audit_log_path =
        command_line->GetSwitchValuePath(switches::kBrowserAgentAuditLog);
    if (candidate.audit_log_path.empty()) {
      LOG(ERROR)
          << "event=browser_agent_service_disabled reason=empty_audit_path";
      return false;
    }
  }
  if (command_line->HasSwitch(switches::kBrowserAgentTraceDir)) {
    candidate.trace_directory =
        command_line->GetSwitchValuePath(switches::kBrowserAgentTraceDir);
    if (candidate.trace_directory.empty()) {
      LOG(ERROR)
          << "event=browser_agent_service_disabled reason=empty_trace_path";
      return false;
    }
  }
  *config = std::move(candidate);
  return true;
}

}  // namespace browser_agent
