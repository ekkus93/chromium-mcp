// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;
namespace content {
class WebContents;
}

namespace browser_agent {

class BrowserAgentMcpServer;

// Profile-scoped owner for the in-process MCP server and browser-native tools.
class BrowserAgentService : public KeyedService {
 public:
  struct Config {
    int mcp_port = -1;
    base::FilePath audit_log_path;
    base::FilePath trace_directory;
  };

  explicit BrowserAgentService(Profile* profile, Config config);
  BrowserAgentService(const BrowserAgentService&) = delete;
  BrowserAgentService& operator=(const BrowserAgentService&) = delete;
  ~BrowserAgentService() override;

  void Shutdown() override;

  const Config& config() const { return config_; }
  const std::string& session_capability() const { return session_capability_; }
  int mcp_port() const { return mcp_port_; }
  base::RepeatingCallback<void(base::DictValue)> CreateNetworkEventCallback(
      content::WebContents* contents);

  // Returns false when the service is not explicitly enabled or a configured
  // audit/trace path is empty.
  static bool IsExplicitlyEnabled();
  static bool GetConfigFromCommandLine(Config* config);

 private:
  Config config_;
  const std::string session_capability_;
  int mcp_port_ = -1;
  std::unique_ptr<BrowserAgentMcpServer> mcp_server_;
  bool is_shutdown_ = false;
};

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_H_
