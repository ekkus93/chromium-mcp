// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_
#define CHROME_COMMON_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace network {
struct ResourceRequest;
struct HttpRequestHeadersUpdateParams;
namespace mojom {
class URLResponseHead;
}
}  // namespace network
namespace net {
struct RedirectInfo;
}

namespace browser_agent {

class BrowserAgentNetworkResponseForwarder;

// Captures native subresource requests and tees response bodies to the page.
class BrowserAgentNetworkThrottle final : public blink::URLLoaderThrottle {
 public:
  using EventCallback = base::RepeatingCallback<void(base::DictValue)>;
  using CaptureEligibilityCallback =
      base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>;

  BrowserAgentNetworkThrottle(EventCallback event_callback,
                              CaptureEligibilityCallback eligibility_callback);
  BrowserAgentNetworkThrottle(const BrowserAgentNetworkThrottle&) = delete;
  BrowserAgentNetworkThrottle& operator=(const BrowserAgentNetworkThrottle&) =
      delete;
  ~BrowserAgentNetworkThrottle() override;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      network::HttpRequestHeadersUpdateParams* headers_update_params) override;
  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* response_head,
                           bool* defer) override;

 private:
  void OnResponseDeferred(
      network::mojom::URLResponseHeadPtr response_head_copy);
  void OnCaptureEligibilityResolved(
      network::mojom::URLResponseHeadPtr response_head_copy,
      bool capture);
  void ResumeWithoutCapture();

  const EventCallback event_callback_;
  const CaptureEligibilityCallback eligibility_callback_;
  std::string request_url_;
  std::string request_id_;
  bool capture_response_ = false;
  bool has_redirected_ = false;
  std::unique_ptr<BrowserAgentNetworkResponseForwarder> response_forwarder_;
  base::WeakPtrFactory<BrowserAgentNetworkThrottle> weak_factory_{this};
};

}  // namespace browser_agent

#endif  // CHROME_COMMON_AGENT_BROWSER_AGENT_NETWORK_THROTTLE_H_
