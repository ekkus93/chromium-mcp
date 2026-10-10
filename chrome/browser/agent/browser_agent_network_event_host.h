// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_EVENT_HOST_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_EVENT_HOST_H_

#include "base/unguessable_token.h"
#include "base/values.h"
#include "chrome/common/agent/browser_agent_network_event_host.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace browser_agent {

class BrowserAgentNetworkEventHost final
    : public chrome::mojom::BrowserAgentNetworkEventHost {
 public:
  explicit BrowserAgentNetworkEventHost(int render_process_host_id);
  ~BrowserAgentNetworkEventHost() override;

  void ShouldCaptureResponse(const base::UnguessableToken& frame_token,
                             ShouldCaptureResponseCallback callback) override;
  void ReportNetworkEvent(const base::UnguessableToken& frame_token,
                          base::DictValue event) override;
  void Clone(mojo::PendingReceiver<
             chrome::mojom::BrowserAgentNetworkEventHost> receiver) override;

 private:
  const int render_process_host_id_;
};

void BindBrowserAgentNetworkEventHost(
    int render_process_host_id,
    mojo::PendingReceiver<chrome::mojom::BrowserAgentNetworkEventHost>
        receiver);

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_NETWORK_EVENT_HOST_H_
