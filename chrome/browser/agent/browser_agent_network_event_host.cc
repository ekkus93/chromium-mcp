// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_network_event_host.h"

#include <memory>

#include "chrome/browser/agent/browser_agent_service.h"
#include "chrome/browser/agent/browser_agent_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace browser_agent {

BrowserAgentNetworkEventHost::BrowserAgentNetworkEventHost(
    int render_process_host_id)
    : render_process_host_id_(render_process_host_id) {}

BrowserAgentNetworkEventHost::~BrowserAgentNetworkEventHost() = default;

void BrowserAgentNetworkEventHost::ShouldCaptureResponse(
    const base::UnguessableToken& frame_token,
    ShouldCaptureResponseCallback callback) {
  auto* process = content::RenderProcessHost::FromID(render_process_host_id_);
  auto* render_frame_host = content::RenderFrameHost::FromFrameToken(
      content::GlobalRenderFrameHostToken(render_process_host_id_,
                                          blink::LocalFrameToken(frame_token)));
  if (!process || !render_frame_host ||
      render_frame_host->GetProcess() != process) {
    std::move(callback).Run(false);
    return;
  }
  auto* profile = Profile::FromBrowserContext(process->GetBrowserContext());
  auto* service = BrowserAgentServiceFactory::GetForProfile(profile);
  auto* web_contents =
      content::WebContents::FromRenderFrameHost(render_frame_host);
  std::move(callback).Run(
      service && web_contents &&
      !service->CreateNetworkEventCallback(web_contents).is_null());
}

void BrowserAgentNetworkEventHost::ReportNetworkEvent(
    const base::UnguessableToken& frame_token,
    base::DictValue event) {
  auto* process = content::RenderProcessHost::FromID(render_process_host_id_);
  if (!process) {
    return;
  }
  auto* render_frame_host = content::RenderFrameHost::FromFrameToken(
      content::GlobalRenderFrameHostToken(
          render_process_host_id_, blink::LocalFrameToken(frame_token)));
  if (!render_frame_host || render_frame_host->GetProcess() != process) {
    return;
  }
  auto* profile = Profile::FromBrowserContext(process->GetBrowserContext());
  auto* service = BrowserAgentServiceFactory::GetForProfile(profile);
  auto* web_contents =
      content::WebContents::FromRenderFrameHost(render_frame_host);
  if (!service || !web_contents) {
    return;
  }
  auto event_callback = service->CreateNetworkEventCallback(web_contents);
  if (event_callback) {
    event_callback.Run(std::move(event));
  }
}

void BrowserAgentNetworkEventHost::Clone(
    mojo::PendingReceiver<chrome::mojom::BrowserAgentNetworkEventHost>
        receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<BrowserAgentNetworkEventHost>(render_process_host_id_),
      std::move(receiver));
}

void BindBrowserAgentNetworkEventHost(
    int render_process_host_id,
    mojo::PendingReceiver<chrome::mojom::BrowserAgentNetworkEventHost>
        receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<BrowserAgentNetworkEventHost>(render_process_host_id),
      std::move(receiver));
}

}  // namespace browser_agent
