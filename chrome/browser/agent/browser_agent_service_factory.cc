// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_service_factory.h"

#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"

namespace browser_agent {

// static
BrowserAgentServiceFactory* BrowserAgentServiceFactory::GetInstance() {
  static base::NoDestructor<BrowserAgentServiceFactory> instance;
  return instance.get();
}

// static
BrowserAgentService* BrowserAgentServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<BrowserAgentService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

BrowserAgentServiceFactory::BrowserAgentServiceFactory()
    : ProfileKeyedServiceFactory("BrowserAgentService",
                                 ProfileSelections::BuildForRegularProfile()) {}

BrowserAgentServiceFactory::~BrowserAgentServiceFactory() = default;

bool BrowserAgentServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

std::unique_ptr<KeyedService>
BrowserAgentServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  BrowserAgentService::Config config;
  if (!BrowserAgentService::GetConfigFromCommandLine(&config)) {
    return nullptr;
  }
  return std::make_unique<BrowserAgentService>(
      Profile::FromBrowserContext(context), std::move(config));
}

}  // namespace browser_agent
