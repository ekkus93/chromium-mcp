// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/scoped_command_line.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/agent/browser_agent_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace browser_agent {
namespace {

TEST(BrowserAgentServiceTest, RequiresFeatureAndExplicitSwitch) {
  content::BrowserTaskEnvironment task_environment;
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(features::kBrowserAgentService);
  base::test::ScopedCommandLine command_line;
  TestingProfile disabled_profile;

  EXPECT_FALSE(BrowserAgentService::IsExplicitlyEnabled());
  EXPECT_EQ(nullptr,
            BrowserAgentServiceFactory::GetForProfile(&disabled_profile));

  command_line.GetProcessCommandLine()->AppendSwitch(
      switches::kEnableBrowserAgentService);
  TestingProfile enabled_profile;
  EXPECT_TRUE(BrowserAgentService::IsExplicitlyEnabled());
  EXPECT_NE(nullptr,
            BrowserAgentServiceFactory::GetForProfile(&enabled_profile));
}

TEST(BrowserAgentServiceTest, FeatureMustAlsoBeEnabled) {
  content::BrowserTaskEnvironment task_environment;
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(features::kBrowserAgentService);
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitch(
      switches::kEnableBrowserAgentService);
  TestingProfile profile;

  EXPECT_FALSE(BrowserAgentService::IsExplicitlyEnabled());
  EXPECT_EQ(nullptr, BrowserAgentServiceFactory::GetForProfile(&profile));
}

TEST(BrowserAgentServiceTest, CreatesNativeControlService) {
  content::BrowserTaskEnvironment task_environment;
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(features::kBrowserAgentService);
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitch(
      switches::kEnableBrowserAgentService);
  TestingProfile profile;

  BrowserAgentService* service =
      BrowserAgentServiceFactory::GetForProfile(&profile);
  ASSERT_NE(nullptr, service);
  service->Shutdown();
}

TEST(BrowserAgentServiceTest, ParsesArtifactPaths) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(features::kBrowserAgentService);
  base::test::ScopedCommandLine command_line;
  base::CommandLine* process_command_line =
      command_line.GetProcessCommandLine();
  process_command_line->AppendSwitch(switches::kEnableBrowserAgentService);
  process_command_line->AppendSwitchPath(
      switches::kBrowserAgentAuditLog,
      base::FilePath(FILE_PATH_LITERAL("agent-audit.jsonl")));
  process_command_line->AppendSwitchPath(
      switches::kBrowserAgentTraceDir,
      base::FilePath(FILE_PATH_LITERAL("agent-traces")));
  BrowserAgentService::Config config;

  ASSERT_TRUE(BrowserAgentService::GetConfigFromCommandLine(&config));
  EXPECT_EQ(base::FilePath(FILE_PATH_LITERAL("agent-audit.jsonl")),
            config.audit_log_path);
  EXPECT_EQ(base::FilePath(FILE_PATH_LITERAL("agent-traces")),
            config.trace_directory);
}

}  // namespace
}  // namespace browser_agent
