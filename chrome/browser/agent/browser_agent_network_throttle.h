// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace network {
struct ResourceRequest;
namespace mojom {
class URLResponseHead;
}
}  // namespace network

namespace browser_agent {

class BrowserAgentNetworkResponseForwarder;

// Captures native subresource requests and tees response bodies to the page.
class BrowserAgentNetworkThrottle final : public blink::URLLoaderThrottle {
 public:
  using EventCallback = base::RepeatingCallback<void(base::DictValue)>;

  explicit BrowserAgentNetworkThrottle(EventCallback event_callback);
  BrowserAgentNetworkThrottle(const BrowserAgentNetworkThrottle&) = delete;
  BrowserAgentNetworkThrottle& operator=(const BrowserAgentNetworkThrottle&) =
      delete;
  ~BrowserAgentNetworkThrottle() override;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* response_head,
                           bool* defer) override;

 private:
  const EventCallback event_callback_;
  std::string request_url_;
  std::string request_id_;
  bool capture_response_ = false;
  std::unique_ptr<BrowserAgentNetworkResponseForwarder> response_forwarder_;
};

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_
