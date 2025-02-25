// Copyright (c) 2019 Vivaldi Technologies AS. All rights reserved

#include "ui/content/vivaldi_tab_check.h"

#include "app/vivaldi_apptools.h"
#include "content/browser/renderer_host/render_widget_host_view_child_frame.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"

const int VivaldiTabCheck::kVivaldiTabObserverContextKey = 0;

// static
bool VivaldiTabCheck::IsVivaldiTab(content::WebContents* web_contents) {
  return web_contents->GetUserData(&kVivaldiTabObserverContextKey);
}
