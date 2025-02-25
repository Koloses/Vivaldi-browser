// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/guest_view/web_view/web_view_permission_helper.h"

#include <utility>

#include "base/bind.h"
#include "base/location.h"
#include "base/metrics/user_metrics.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/values.h"
#include "components/guest_view/browser/guest_view_event.h"
#include "content/public/browser/render_process_host.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/guest_view/web_view/web_view_constants.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "extensions/browser/guest_view/web_view/web_view_permission_helper_delegate.h"
#include "extensions/browser/guest_view/web_view/web_view_permission_types.h"
#include "ppapi/buildflags/buildflags.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"

#include "base/command_line.h"
#include "browser/vivaldi_browser_finder.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/extensions/api/tab_capture/tab_capture_registry.h"
#include "chrome/browser/plugins/flash_temporary_permission_tracker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/extensions/extension_constants.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "extensions/api/tabs/tabs_private_api.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_guest.h"

#include "app/vivaldi_apptools.h"
#include "extensions/helper/vivaldi_app_helper.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"

using base::UserMetricsAction;
using content::BrowserPluginGuestDelegate;
using guest_view::GuestViewEvent;

namespace extensions {

namespace {
static std::string PermissionTypeToString(WebViewPermissionType type) {
  switch (type) {
    case WEB_VIEW_PERMISSION_TYPE_DOWNLOAD:
      return webview::kPermissionTypeDownload;
    case WEB_VIEW_PERMISSION_TYPE_FILESYSTEM:
      return webview::kPermissionTypeFileSystem;
    case WEB_VIEW_PERMISSION_TYPE_FULLSCREEN:
      return webview::kPermissionTypeFullscreen;
    case WEB_VIEW_PERMISSION_TYPE_GEOLOCATION:
      return webview::kPermissionTypeGeolocation;
    case WEB_VIEW_PERMISSION_TYPE_NOTIFICATION:
      return "notifications";
    case WEB_VIEW_PERMISSION_TYPE_JAVASCRIPT_DIALOG:
      return webview::kPermissionTypeDialog;
    case WEB_VIEW_PERMISSION_TYPE_LOAD_PLUGIN:
      return webview::kPermissionTypeLoadPlugin;
    case WEB_VIEW_PERMISSION_TYPE_MEDIA:
      return webview::kPermissionTypeMedia;
    case WEB_VIEW_PERMISSION_TYPE_NEW_WINDOW:
      return webview::kPermissionTypeNewWindow;
    case WEB_VIEW_PERMISSION_TYPE_POINTER_LOCK:
      return webview::kPermissionTypePointerLock;
    case WEB_VIEW_PERMISSION_TYPE_CAMERA:
      return "camera";
    case WEB_VIEW_PERMISSION_TYPE_MICROPHONE:
      return "microphone";
    case WEB_VIEW_PERMISSION_TYPE_MICROPHONE_AND_CAMERA:
      return "microphone_and_camera";
    default:
      NOTREACHED();
      return std::string();
  }
}

// static
void RecordUserInitiatedUMA(
    const WebViewPermissionHelper::PermissionResponseInfo& info,
    bool allow) {
  if (allow) {
    // Note that |allow| == true means the embedder explicitly allowed the
    // request. For some requests they might still fail. An example of such
    // scenario would be: an embedder allows geolocation request but doesn't
    // have geolocation access on its own.
    switch (info.permission_type) {
      case WEB_VIEW_PERMISSION_TYPE_DOWNLOAD:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.Download"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_FILESYSTEM:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.FileSystem"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_FULLSCREEN:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.Fullscreen"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_GEOLOCATION:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.Geolocation"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_JAVASCRIPT_DIALOG:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.JSDialog"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_LOAD_PLUGIN:
        base::RecordAction(
            UserMetricsAction("WebView.Guest.PermissionAllow.PluginLoad"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_MEDIA:
        base::RecordAction(UserMetricsAction("WebView.PermissionAllow.Media"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_NEW_WINDOW:
        base::RecordAction(
            UserMetricsAction("BrowserPlugin.PermissionAllow.NewWindow"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_POINTER_LOCK:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionAllow.PointerLock"));
        break;
      default:
        break;
    }
  } else {
    switch (info.permission_type) {
      case WEB_VIEW_PERMISSION_TYPE_DOWNLOAD:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.Download"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_FILESYSTEM:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.FileSystem"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_FULLSCREEN:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.Fullscreen"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_GEOLOCATION:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.Geolocation"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_JAVASCRIPT_DIALOG:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.JSDialog"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_LOAD_PLUGIN:
        base::RecordAction(
            UserMetricsAction("WebView.Guest.PermissionDeny.PluginLoad"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_MEDIA:
        base::RecordAction(UserMetricsAction("WebView.PermissionDeny.Media"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_NEW_WINDOW:
        base::RecordAction(
            UserMetricsAction("BrowserPlugin.PermissionDeny.NewWindow"));
        break;
      case WEB_VIEW_PERMISSION_TYPE_POINTER_LOCK:
        base::RecordAction(
            UserMetricsAction("WebView.PermissionDeny.PointerLock"));
        break;
      default:
        break;
    }
  }
}

} // namespace

WebViewPermissionHelper::WebViewPermissionHelper(WebViewGuest* web_view_guest)
    : content::WebContentsObserver(web_view_guest->web_contents()),
      next_permission_request_id_(guest_view::kInstanceIDNone),
      web_view_guest_(web_view_guest),
      default_media_access_permission_(false) {
  web_view_permission_helper_delegate_.reset(
      ExtensionsAPIClient::Get()->CreateWebViewPermissionHelperDelegate(this));
}

WebViewPermissionHelper::WebViewPermissionHelper(GuestViewBase* web_view_guest)
    : content::WebContentsObserver(web_view_guest->web_contents()),
      next_permission_request_id_(guest_view::kInstanceIDNone),
      web_view_guest_(web_view_guest),
      weak_factory_(this) {
      web_view_permission_helper_delegate_.reset(
          ExtensionsAPIClient::Get()->CreateWebViewPermissionHelperDelegate(
              this));
}

WebViewPermissionHelper::~WebViewPermissionHelper() {
}

// static
WebViewPermissionHelper* WebViewPermissionHelper::FromFrameID(
    int render_process_id,
    int render_frame_id) {
  WebViewGuest* web_view_guest = WebViewGuest::FromFrameID(
      render_process_id, render_frame_id);
  if (!web_view_guest) {
    return NULL;
  }
  return web_view_guest->web_view_permission_helper_.get();
}

// static
WebViewPermissionHelper* WebViewPermissionHelper::FromWebContents(
      content::WebContents* web_contents) {
  // This can be a MimeHandlerViewGuest, used to show PDFs. In that case use the
  // owner webcontents which will give us a WebViewGuest.
  MimeHandlerViewGuest* mime_view_guest =
      MimeHandlerViewGuest::FromWebContents(web_contents);
  if (mime_view_guest) {
    WebViewGuest* web_view_guest =
        WebViewGuest::FromWebContents(mime_view_guest->owner_web_contents());
    return web_view_guest->web_view_permission_helper_.get();
  }
  WebViewGuest* web_view_guest = WebViewGuest::FromWebContents(web_contents);
  if (!web_view_guest)
      return NULL;
  return web_view_guest->web_view_permission_helper_.get();
}

#if BUILDFLAG(ENABLE_PLUGINS)
bool WebViewPermissionHelper::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* render_frame_host) {
  return web_view_permission_helper_delegate_->OnMessageReceived(
      message, render_frame_host);
}
#endif  // BUILDFLAG(ENABLE_PLUGINS)

void WebViewPermissionHelper::RequestMediaAccessPermission(
    content::WebContents* source,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  // Vivaldi
  // If this is a TabCast request.
  if (request.video_type == blink::mojom::MediaStreamType::GUM_TAB_VIDEO_CAPTURE ||
      request.audio_type == blink::mojom::MediaStreamType::GUM_TAB_AUDIO_CAPTURE) {
    // Only allow the stable Google cast component extension...
    std::string extension_id = request.security_origin.host();
    if (extension_id == extension_misc::kMediaRouterStableExtensionId) {
      blink::MediaStreamDevices devices;
      extensions::TabCaptureRegistry* const tab_capture_registry =
          extensions::TabCaptureRegistry::Get(source->GetBrowserContext());
      // and doublecheck that this came from the correct renderer.
      if (tab_capture_registry &&
          tab_capture_registry->VerifyRequest(request.render_process_id,
                                              request.render_frame_id,
                                              extension_id)) {
        if (request.audio_type == blink::mojom::MediaStreamType::GUM_TAB_AUDIO_CAPTURE) {
          devices.push_back(
              blink::MediaStreamDevice(blink::mojom::MediaStreamType::GUM_TAB_AUDIO_CAPTURE,
                                       std::string(), std::string()));
        }
        if (request.video_type == blink::mojom::MediaStreamType::GUM_TAB_VIDEO_CAPTURE) {
          devices.push_back(
              blink::MediaStreamDevice(blink::mojom::MediaStreamType::GUM_TAB_VIDEO_CAPTURE,
                                       std::string(), std::string()));
        }
      }
      blink::mojom::MediaStreamRequestResult result =
          blink::mojom::MediaStreamRequestResult::INVALID_STATE;

      std::unique_ptr<content::MediaStreamUI> ui;
      if (!devices.empty()) {
        result = blink::mojom::MediaStreamRequestResult::OK;
        ui = MediaCaptureDevicesDispatcher::GetInstance()
          ->GetMediaStreamCaptureIndicator()
          ->RegisterMediaStream(source, devices);
      }

      std::move(callback).Run(devices, result, std::move(ui));
      return;
    }
  }

  extensions::VivaldiAppHelper* helper =
      extensions::VivaldiAppHelper::FromWebContents(web_view_guest()->
        embedder_web_contents());
  if (helper) do {
    Profile* profile = Profile::FromBrowserContext(source->GetBrowserContext());

    ContentSetting audio_setting = CONTENT_SETTING_DEFAULT;
    ContentSetting camera_setting = CONTENT_SETTING_DEFAULT;

    if (request.audio_type != blink::mojom::MediaStreamType::NO_SERVICE) {
      audio_setting = HostContentSettingsMapFactory::GetForProfile(profile)->
        GetContentSetting(
            request.security_origin, GURL(),
            CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, std::string());
      if (audio_setting != CONTENT_SETTING_ALLOW &&
          audio_setting != CONTENT_SETTING_BLOCK)
        break;
    }
    if (request.video_type  != blink::mojom::MediaStreamType::NO_SERVICE) {
      camera_setting = HostContentSettingsMapFactory::GetForProfile(profile)->
        GetContentSetting(
            request.security_origin, GURL(),
            CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, std::string());
      if (camera_setting != CONTENT_SETTING_ALLOW &&
          camera_setting != CONTENT_SETTING_BLOCK)
        break;
    }

    // Only default (not requested), allow and block allowed.
    // Others are "always ask".
    if (audio_setting == CONTENT_SETTING_BLOCK ||
        camera_setting == CONTENT_SETTING_BLOCK) {
      std::move(callback).Run(blink::MediaStreamDevices(),
                              blink::mojom::MediaStreamRequestResult::PERMISSION_DENIED,
                              std::unique_ptr<content::MediaStreamUI>());
      return;
    }
    if (audio_setting == CONTENT_SETTING_ALLOW ||
        camera_setting == CONTENT_SETTING_ALLOW) {
      web_view_guest()
          ->embedder_web_contents()
          ->GetDelegate()
          ->RequestMediaAccessPermission(
              web_view_guest()->embedder_web_contents(), request,
              std::move(callback));
      return;
    }
  } while (false);

  WebViewPermissionType request_type = WEB_VIEW_PERMISSION_TYPE_MEDIA;

  if (::vivaldi::IsVivaldiRunning()) {
    // camera , microphone and microphone_and_camera
    if (request.audio_type == blink::mojom::MediaStreamType::DEVICE_AUDIO_CAPTURE &&
        request.video_type == blink::mojom::MediaStreamType::DEVICE_VIDEO_CAPTURE) {
      request_type = WEB_VIEW_PERMISSION_TYPE_MICROPHONE_AND_CAMERA;
    } else if (request.video_type == blink::mojom::MediaStreamType::DEVICE_VIDEO_CAPTURE) {
      request_type = WEB_VIEW_PERMISSION_TYPE_CAMERA;
    } else if (request.audio_type == blink::mojom::MediaStreamType::DEVICE_AUDIO_CAPTURE) {
      request_type = WEB_VIEW_PERMISSION_TYPE_MICROPHONE;
    }
  }
  // End Vivaldi

  base::DictionaryValue request_info;
  request_info.SetString(guest_view::kUrl, request.security_origin.spec());
  RequestPermission(
      request_type, request_info,
      base::BindOnce(&WebViewPermissionHelper::OnMediaPermissionResponse,
                     weak_factory_.GetWeakPtr(), request, std::move(callback)),
      default_media_access_permission_);
}

bool WebViewPermissionHelper::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const GURL& security_origin,
    blink::mojom::MediaStreamType type) {
  if (!web_view_guest()->attached() ||
      !web_view_guest()->embedder_web_contents()->GetDelegate()) {
    return false;
  }
  return web_view_guest()
      ->embedder_web_contents()
      ->GetDelegate()
      ->CheckMediaAccessPermission(render_frame_host, security_origin, type);
}

void WebViewPermissionHelper::OnMediaPermissionResponse(
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback,
    bool allow,
    const std::string& user_input) {
  ContentSettingsPattern primary_pattern =
      ContentSettingsPattern::FromURLNoWildcard(request.security_origin);

  extensions::VivaldiAppHelper* helper =
      extensions::VivaldiAppHelper::FromWebContents(
          web_view_guest()->embedder_web_contents());
  if (helper) {
    Browser* browser = ::vivaldi::FindBrowserWithWebContents(web_contents());
    DCHECK(browser);
    Profile* profile = browser->profile();

    if (request.audio_type != blink::mojom::MediaStreamType::NO_SERVICE) {
      HostContentSettingsMapFactory::GetForProfile(profile)
          ->SetContentSettingCustomScope(
              primary_pattern, ContentSettingsPattern::Wildcard(),
              CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, std::string(),
              allow ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_BLOCK);
    }
    if (request.video_type  != blink::mojom::MediaStreamType::NO_SERVICE) {
      HostContentSettingsMapFactory::GetForProfile(profile)
          ->SetContentSettingCustomScope(
              primary_pattern, ContentSettingsPattern::Wildcard(),
              CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, std::string(),
              allow ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_BLOCK);
    }
  }

  if (!allow) {
    std::move(callback).Run(
        blink::MediaStreamDevices(),
        blink::mojom::MediaStreamRequestResult::PERMISSION_DENIED,
        std::unique_ptr<content::MediaStreamUI>());
    return;
  }
  if (!web_view_guest()->attached() ||
      !web_view_guest()->embedder_web_contents()->GetDelegate()) {
    std::move(callback).Run(
        blink::MediaStreamDevices(),
        blink::mojom::MediaStreamRequestResult::INVALID_STATE,
        std::unique_ptr<content::MediaStreamUI>());
    return;
  }

  web_view_guest()
      ->embedder_web_contents()
      ->GetDelegate()
      ->RequestMediaAccessPermission(web_view_guest()->embedder_web_contents(),
                                     request, std::move(callback));
}

void WebViewPermissionHelper::CanDownload(
    const GURL& url,
    const std::string& request_method,
    base::OnceCallback<void(bool)> callback) {
  web_view_permission_helper_delegate_->SetDownloadInformation(download_info_);
  web_view_permission_helper_delegate_->CanDownload(url, request_method,
                                                    std::move(callback));
}

void WebViewPermissionHelper::RequestPointerLockPermission(
    bool user_gesture,
    bool last_unlocked_by_target,
    const base::Callback<void(bool)>& callback) {
  web_view_permission_helper_delegate_->RequestPointerLockPermission(
      user_gesture, last_unlocked_by_target, callback);
}

void WebViewPermissionHelper::RequestGeolocationPermission(
    int bridge_id,
    const GURL& requesting_frame,
    bool user_gesture,
    base::OnceCallback<void(bool)> callback) {
  web_view_permission_helper_delegate_->RequestGeolocationPermission(
      bridge_id, requesting_frame, user_gesture, std::move(callback));
}

void WebViewPermissionHelper::CancelGeolocationPermissionRequest(
    int bridge_id) {
  web_view_permission_helper_delegate_->CancelGeolocationPermissionRequest(
      bridge_id);
}

void WebViewPermissionHelper::RequestNotificationPermission(
    int bridge_id,
    const GURL& requesting_frame,
    bool user_gesture,
    const base::Callback<void(bool)>& callback) {
  web_view_permission_helper_delegate_->RequestNotificationPermission(
      bridge_id, requesting_frame, user_gesture, callback);
}

void WebViewPermissionHelper::CancelNotificationPermissionRequest(
    int bridge_id) {
  web_view_permission_helper_delegate_->CancelNotificationPermissionRequest(
      bridge_id);
}

void WebViewPermissionHelper::RequestFileSystemPermission(
    const GURL& url,
    bool allowed_by_default,
    const base::Callback<void(bool)>& callback) {
  web_view_permission_helper_delegate_->RequestFileSystemPermission(
      url, allowed_by_default, callback);
}

void WebViewPermissionHelper::FileSystemAccessedAsync(int render_process_id,
                                                      int render_frame_id,
                                                      int request_id,
                                                      const GURL& url,
                                                      bool blocked_by_policy) {
  web_view_permission_helper_delegate_->FileSystemAccessedAsync(
      render_process_id, render_frame_id, request_id, url, blocked_by_policy);
}

int WebViewPermissionHelper::RequestPermission(
    WebViewPermissionType permission_type,
    const base::DictionaryValue& request_info,
    PermissionResponseCallback callback,
    bool allowed_by_default) {
  // If there are too many pending permission requests then reject this request.
  if (pending_permission_requests_.size() >=
      webview::kMaxOutstandingPermissionRequests) {
    // Let the stack unwind before we deny the permission request so that
    // objects held by the permission request are not destroyed immediately
    // after creation. This is to allow those same objects to be accessed again
    // in the same scope without fear of use after freeing.
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), allowed_by_default, std::string()));
    return webview::kInvalidPermissionRequestID;
  }

  int request_id = next_permission_request_id_++;
  pending_permission_requests_[request_id] = PermissionResponseInfo(
      std::move(callback), permission_type, allowed_by_default);
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetKey(webview::kRequestInfo, request_info.Clone());
  args->SetInteger(webview::kRequestId, request_id);
  switch (permission_type) {
    case WEB_VIEW_PERMISSION_TYPE_NEW_WINDOW: {
      web_view_guest_->DispatchEventToView(std::make_unique<GuestViewEvent>(
          webview::kEventNewWindow, std::move(args)));
      break;
    }
    case WEB_VIEW_PERMISSION_TYPE_JAVASCRIPT_DIALOG: {
      web_view_guest_->DispatchEventToView(std::make_unique<GuestViewEvent>(
          webview::kEventDialog, std::move(args)));
      break;
    }
    default: {
      args->SetString(webview::kPermission,
                      PermissionTypeToString(permission_type));
      web_view_guest_->DispatchEventToView(std::make_unique<GuestViewEvent>(
          webview::kEventPermissionRequest, std::move(args)));
      break;
    }
  }
  return request_id;
}

WebViewPermissionHelper::SetPermissionResult
WebViewPermissionHelper::SetPermission(
    int request_id,
    PermissionResponseAction action,
    const std::string& user_input) {
  auto request_itr = pending_permission_requests_.find(request_id);

  if (request_itr == pending_permission_requests_.end())
    return SET_PERMISSION_INVALID;

  PermissionResponseInfo& info = request_itr->second;
  bool allow = (action == ALLOW) ||
      ((action == DEFAULT) && info.allowed_by_default);

  // Will temporary enable Flash for this site.
  if (allow && info.permission_type == WEB_VIEW_PERMISSION_TYPE_LOAD_PLUGIN) {

    Browser* browser = ::vivaldi::FindBrowserWithWebContents(web_contents());
    DCHECK(browser);
    Profile* profile = browser->profile();

    FlashTemporaryPermissionTracker::Get(profile)->FlashEnabledForWebContents(
      web_contents());
  }

  std::move(info.callback).Run(allow, user_input);

  // Only record user initiated (i.e. non-default) actions.
  if (action != DEFAULT)
    RecordUserInitiatedUMA(info, allow);

  pending_permission_requests_.erase(request_itr);

  return allow ? SET_PERMISSION_ALLOWED : SET_PERMISSION_DENIED;
}

void WebViewPermissionHelper::CancelPendingPermissionRequest(int request_id) {
  auto request_itr = pending_permission_requests_.find(request_id);

  if (request_itr == pending_permission_requests_.end())
    return;

  pending_permission_requests_.erase(request_itr);
}

WebViewPermissionHelper::PermissionResponseInfo::PermissionResponseInfo()
    : permission_type(WEB_VIEW_PERMISSION_TYPE_UNKNOWN),
      allowed_by_default(false) {
}

WebViewPermissionHelper::PermissionResponseInfo::PermissionResponseInfo(
    PermissionResponseCallback callback,
    WebViewPermissionType permission_type,
    bool allowed_by_default)
    : callback(std::move(callback)),
      permission_type(permission_type),
      allowed_by_default(allowed_by_default) {}

WebViewPermissionHelper::PermissionResponseInfo&
WebViewPermissionHelper::PermissionResponseInfo::operator=(
    WebViewPermissionHelper::PermissionResponseInfo&& other) = default;

WebViewPermissionHelper::PermissionResponseInfo::~PermissionResponseInfo() {}

}  // namespace extensions
