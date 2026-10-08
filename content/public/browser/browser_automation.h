// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_BROWSER_AUTOMATION_H_
#define CONTENT_PUBLIC_BROWSER_BROWSER_AUTOMATION_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "content/common/content_export.h"

namespace content {

class RenderFrameHost;

// Evaluates JavaScript in a frame's main world. Callers must already have
// explicit authority to automate the frame and must enforce that authority.
CONTENT_EXPORT void ExecuteJavaScriptForAutomation(
    RenderFrameHost* render_frame_host,
    const std::u16string& javascript,
    base::OnceCallback<void(base::Value)> callback);

// Supplies local files to the next file input chooser opened by this frame.
// The browser's normal file chooser completion path grants renderer access.
CONTENT_EXPORT void SetFilesForNextFileChooserForAutomation(
    RenderFrameHost* render_frame_host,
    std::vector<base::FilePath> paths);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_BROWSER_AUTOMATION_H_
