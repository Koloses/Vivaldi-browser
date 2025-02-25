// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/vivaldi_ui_utils.h"

#include <string>
#include <vector>

#include "app/vivaldi_constants.h"
#include "base/json/json_reader.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/sessions/session_tab_helper.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/scrollbar_size.h"
#include "ui/gfx/skbitmap_operations.h"
#include "ui/vivaldi_browser_window.h"
#include "skia/ext/image_operations.h"
#include "skia/ext/platform_canvas.h"
#include "ui/views/widget/widget.h"

namespace vivaldi {
namespace ui_tools {

/*static*/
extensions::WebViewGuest* GetActiveWebViewGuest() {
  Browser* browser = chrome::FindLastActive();
  if (!browser)
    return nullptr;

  return GetActiveWebGuestFromBrowser(browser);
}

/*static*/
extensions::WebViewGuest* GetActiveWebViewGuest(
    extensions::NativeAppWindow* app_window) {
  Browser* browser =
      chrome::FindBrowserWithWindow(app_window->GetNativeWindow());

  if (!browser)
    return nullptr;

  return GetActiveWebGuestFromBrowser(browser);
}

/*static*/
extensions::WebViewGuest* GetActiveWebGuestFromBrowser(Browser* browser) {
  content::WebContents* active_web_contents =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!active_web_contents)
    return nullptr;

  return extensions::WebViewGuest::FromWebContents(active_web_contents);
}

/*static*/
VivaldiBrowserWindow* GetActiveAppWindow() {
#if defined(OS_WIN) || defined(OS_LINUX)
  Browser* browser = chrome::FindLastActive();
  if (browser && browser->is_vivaldi())
    return static_cast<VivaldiBrowserWindow*>(browser->window());
#endif
  return nullptr;
}

content::WebContents* GetWebContentsFromTabStrip(
    int tab_id,
    content::BrowserContext* browser_context, std::string* error) {
  content::WebContents* contents = nullptr;
  bool include_incognito = true;
  Browser* browser;
  int tab_index;
  extensions::ExtensionTabUtil::GetTabById(tab_id, browser_context,
                                           include_incognito, &browser, NULL,
                                           &contents, &tab_index);
  if (error && !contents) {
    *error = "Failed to find a tab with id " + std::to_string(tab_id);
  }
  return contents;
}

bool IsOutsideAppWindow(int screen_x, int screen_y) {
  gfx::Point screen_point(screen_x, screen_y);

  bool outside = true;
  for (auto* browser : *BrowserList::GetInstance()) {
    gfx::Rect rect =
        browser->is_devtools() ? gfx::Rect() : browser->window()->GetBounds();
    if (rect.Contains(screen_point)) {
      outside = false;
      break;
    }
  }
  return outside;
}

bool EncodeBitmap(const SkBitmap& bitmap,
                  std::vector<unsigned char>* data,
                  std::string* mime_type,
                  extensions::api::extension_types::ImageFormat image_format,
                  int image_quality) {
  bool encoded = false;

  switch (image_format) {
    case extensions::api::extension_types::IMAGE_FORMAT_JPEG:
      if (bitmap.getPixels()) {
        encoded = gfx::JPEGCodec::Encode(bitmap, image_quality, data);
        *mime_type = "image/jpeg";  // kMimeTypeJpeg;
        if (!encoded) {
          LOG(ERROR) << "Failed to encode bitmap as jpeg";
        }
      } else {
        LOG(ERROR) << "Cannot encode empty bitmap as jpeg";
      }
      break;
    case extensions::api::extension_types::IMAGE_FORMAT_PNG:
      encoded =
          gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                            true,  // Discard transparency.
                                            data);
      *mime_type = "image/png";  // kMimeTypePng;
      if (!encoded) {
        LOG(ERROR) << "Failed to encode bitmap as png";
      }
      break;
    default:
      NOTREACHED() << "Invalid image format.";
  }

  return encoded;
}

bool IsMainVivaldiBrowserWindow(Browser* browser) {
  base::JSONParserOptions options = base::JSON_PARSE_RFC;
  base::Optional<base::Value> json =
      base::JSONReader::Read(browser->ext_data(), options);
  base::DictionaryValue* dict = NULL;
  std::string window_type;
  if (json && json->GetAsDictionary(&dict)) {
    dict->GetString("windowType", &window_type);
    // Don't track popup windows (like settings) in the session.
    // We have "", "popup" and "settings".
    // TODO(pettern): Popup windows still rely on extData, this
    // should go away and we should use the type sent to the apis
    // instead.
    if (window_type == "popup" || window_type == "settings") {
      return false;
    }
  }
  if (static_cast<VivaldiBrowserWindow*>(browser->window())->type() ==
      VivaldiBrowserWindow::WindowType::NORMAL) {
    return true;
  }
  return false;
}

Browser* FindBrowserForPinnedTabs(Browser* current_browser) {
  if (current_browser->profile()->IsOffTheRecord()) {
    // Pinned tabs can never be moved to another browser
    return nullptr;
  }
  for (auto* browser : *BrowserList::GetInstance()) {
    if (browser == current_browser) {
      continue;
    }
    if (!browser->window()) {
      continue;
    }
    if (browser->type() != current_browser->type()) {
      continue;
    }
    if (browser->is_devtools()) {
      continue;
    }
    // Only move within the same profile.
    if (current_browser->profile() != browser->profile()) {
      continue;
    }
    if (!IsMainVivaldiBrowserWindow(browser)) {
      continue;
    }
    return browser;
  }
  return nullptr;
}

// Based on TabsMoveFunction::MoveTab() but greatly simplified.
bool MoveTabToWindow(Browser* source_browser,
                     Browser* target_browser,
                     int tab_index,
                     int* new_index,
                     int iteration) {
  DCHECK(source_browser != target_browser);

  // Insert the tabs one after another.
  *new_index += iteration;

  TabStripModel* source_tab_strip = source_browser->tab_strip_model();
  std::unique_ptr<content::WebContents> web_contents =
      source_tab_strip->DetachWebContentsAt(tab_index);
  if (!web_contents) {
    return false;
  }
  TabStripModel* target_tab_strip = target_browser->tab_strip_model();

  // Clamp move location to the last position.
  // This is ">" because it can append to a new index position.
  // -1 means set the move location to the last position.
  if (*new_index > target_tab_strip->count() || *new_index < 0)
    *new_index = target_tab_strip->count();

  target_tab_strip->InsertWebContentsAt(
    *new_index, std::move(web_contents), TabStripModel::ADD_PINNED);

  return true;
}

bool GetTabById(int tab_id, content::WebContents** contents, int* index) {
  for (auto* target_browser : *BrowserList::GetInstance()) {
    TabStripModel* target_tab_strip = target_browser->tab_strip_model();
    for (int i = 0; i < target_tab_strip->count(); ++i) {
      content::WebContents* target_contents =
          target_tab_strip->GetWebContentsAt(i);
      if (SessionTabHelper::IdForTab(target_contents).id() == tab_id) {
        if (contents)
          *contents = target_contents;
        if (index)
          *index = i;
        return true;
      }
    }
  }
  return false;
}

}  // namespace ui_tools
}  // namespace vivaldi
