// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/content_settings/tab_specific_content_settings.h"

#include <list>
#include <vector>

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/browsing_data/browsing_data_appcache_helper.h"
#include "chrome/browser/browsing_data/browsing_data_cookie_helper.h"
#include "chrome/browser/browsing_data/browsing_data_database_helper.h"
#include "chrome/browser/browsing_data/browsing_data_file_system_helper.h"
#include "chrome/browser/browsing_data/browsing_data_indexed_db_helper.h"
#include "chrome/browser/browsing_data/browsing_data_local_storage_helper.h"
#include "chrome/browser/browsing_data/cookies_tree_model.h"
#include "chrome/browser/content_settings/chrome_content_settings_utils.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/content_settings_renderer.mojom.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/renderer_configuration.mojom.h"
#include "components/content_settings/core/browser/content_settings_details.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/content_settings_utils.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/common/content_constants.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/canonical_cookie.h"
#include "storage/common/fileapi/file_system_types.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/origin.h"

#include "extensions/buildflags/buildflags.h"
#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#endif

using content::BrowserThread;
using content::NavigationController;
using content::NavigationEntry;
using content::WebContents;

namespace {

static TabSpecificContentSettings* GetForWCGetter(
    const base::Callback<content::WebContents*(void)>& wc_getter) {
  WebContents* web_contents = wc_getter.Run();
  if (!web_contents)
    return nullptr;

  return TabSpecificContentSettings::FromWebContents(web_contents);
}

bool ShouldSendUpdatedContentSettingsRulesToRenderer(
    ContentSettingsType content_type) {
  // CONTENT_SETTINGS_TYPE_DEFAULT signals that multiple content settings may
  // have been updated, e.g. by the PolicyProvider. This should always be sent
  // to the renderer in case a relevant setting is updated.
  if (content_type == CONTENT_SETTINGS_TYPE_DEFAULT)
    return true;

  return RendererContentSettingRules::IsRendererContentSetting((content_type));
}

}  // namespace

TabSpecificContentSettings::SiteDataObserver::SiteDataObserver(
    TabSpecificContentSettings* tab_specific_content_settings)
    : tab_specific_content_settings_(tab_specific_content_settings) {
  tab_specific_content_settings_->AddSiteDataObserver(this);
}

TabSpecificContentSettings::SiteDataObserver::~SiteDataObserver() {
  if (tab_specific_content_settings_)
    tab_specific_content_settings_->RemoveSiteDataObserver(this);
}

void TabSpecificContentSettings::SiteDataObserver::ContentSettingsDestroyed() {
  tab_specific_content_settings_ = NULL;
}

TabSpecificContentSettings::TabSpecificContentSettings(WebContents* tab)
    : content::WebContentsObserver(tab),
      map_(HostContentSettingsMapFactory::GetForProfile(
          Profile::FromBrowserContext(tab->GetBrowserContext()))),
      allowed_local_shared_objects_(
          Profile::FromBrowserContext(tab->GetBrowserContext())),
      blocked_local_shared_objects_(
          Profile::FromBrowserContext(tab->GetBrowserContext())),
      geolocation_usages_state_(map_, CONTENT_SETTINGS_TYPE_GEOLOCATION),
      midi_usages_state_(map_, CONTENT_SETTINGS_TYPE_MIDI_SYSEX),
      pending_protocol_handler_(ProtocolHandler::EmptyProtocolHandler()),
      previous_protocol_handler_(ProtocolHandler::EmptyProtocolHandler()),
      pending_protocol_handler_setting_(CONTENT_SETTING_DEFAULT),
      load_plugins_link_enabled_(true),
      microphone_camera_state_(MICROPHONE_CAMERA_NOT_ACCESSED),
      observer_(this) {
  ClearContentSettingsExceptForNavigationRelatedSettings();
  ClearNavigationRelatedContentSettings();

  observer_.Add(map_);
}

TabSpecificContentSettings::~TabSpecificContentSettings() {
  for (SiteDataObserver& observer : observer_list_)
    observer.ContentSettingsDestroyed();
}

TabSpecificContentSettings* TabSpecificContentSettings::GetForFrame(
    int render_process_id,
    int render_frame_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(
      render_process_id, render_frame_id);
  WebContents* web_contents = WebContents::FromRenderFrameHost(frame);
  if (!web_contents)
    return nullptr;

  return TabSpecificContentSettings::FromWebContents(web_contents);
}

// static
void TabSpecificContentSettings::WebDatabaseAccessed(
    int render_process_id,
    int render_frame_id,
    const GURL& url,
    bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings = GetForFrame(
      render_process_id, render_frame_id);
  if (settings)
    settings->OnWebDatabaseAccessed(url, blocked_by_policy);
}

// static
void TabSpecificContentSettings::DOMStorageAccessed(int render_process_id,
                                                    int render_frame_id,
                                                    const GURL& url,
                                                    bool local,
                                                    bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings = GetForFrame(
      render_process_id, render_frame_id);
  if (settings)
    settings->OnLocalStorageAccessed(url, local, blocked_by_policy);
}

// static
void TabSpecificContentSettings::IndexedDBAccessed(
    int render_process_id,
    int render_frame_id,
    const GURL& url,
    bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings = GetForFrame(
      render_process_id, render_frame_id);
  if (settings)
    settings->OnIndexedDBAccessed(url, blocked_by_policy);
}

// static
void TabSpecificContentSettings::CacheStorageAccessed(int render_process_id,
                                                      int render_frame_id,
                                                      const GURL& url,
                                                      bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings =
      GetForFrame(render_process_id, render_frame_id);
  if (settings)
    settings->OnCacheStorageAccessed(url, blocked_by_policy);
}

// static
void TabSpecificContentSettings::FileSystemAccessed(int render_process_id,
                                                    int render_frame_id,
                                                    const GURL& url,
                                                    bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings = GetForFrame(
      render_process_id, render_frame_id);
  if (settings)
    settings->OnFileSystemAccessed(url, blocked_by_policy);
}

// static
void TabSpecificContentSettings::ServiceWorkerAccessed(
    const base::Callback<content::WebContents*(void)>& wc_getter,
    const GURL& scope,
    bool blocked_by_policy_javascript,
    bool blocked_by_policy_cookie) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings = GetForWCGetter(wc_getter);
  if (settings)
    settings->OnServiceWorkerAccessed(scope, blocked_by_policy_javascript,
                                      blocked_by_policy_cookie);
}

// static
void TabSpecificContentSettings::SharedWorkerAccessed(
    int render_process_id,
    int render_frame_id,
    const GURL& worker_url,
    const std::string& name,
    const url::Origin& constructor_origin,
    bool blocked_by_policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabSpecificContentSettings* settings =
      GetForFrame(render_process_id, render_frame_id);
  if (settings)
    settings->OnSharedWorkerAccessed(worker_url, name, constructor_origin,
                                     blocked_by_policy);
}

bool TabSpecificContentSettings::IsContentBlocked(
    ContentSettingsType content_type) const {
  DCHECK_NE(CONTENT_SETTINGS_TYPE_GEOLOCATION, content_type)
      << "Geolocation settings handled by ContentSettingGeolocationImageModel";
  DCHECK_NE(CONTENT_SETTINGS_TYPE_NOTIFICATIONS, content_type)
      << "Notifications settings handled by "
      << "ContentSettingsNotificationsImageModel";
  DCHECK_NE(CONTENT_SETTINGS_TYPE_AUTOMATIC_DOWNLOADS, content_type)
      << "Automatic downloads handled by DownloadRequestLimiter";

  if (content_type == CONTENT_SETTINGS_TYPE_IMAGES ||
      content_type == CONTENT_SETTINGS_TYPE_JAVASCRIPT ||
      content_type == CONTENT_SETTINGS_TYPE_PLUGINS ||
      content_type == CONTENT_SETTINGS_TYPE_COOKIES ||
      content_type == CONTENT_SETTINGS_TYPE_POPUPS ||
      content_type == CONTENT_SETTINGS_TYPE_MIXEDSCRIPT ||
      content_type == CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC ||
      content_type == CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA ||
      content_type == CONTENT_SETTINGS_TYPE_PPAPI_BROKER ||
      content_type == CONTENT_SETTINGS_TYPE_MIDI_SYSEX ||
      content_type == CONTENT_SETTINGS_TYPE_ADS ||
      content_type == CONTENT_SETTINGS_TYPE_SOUND ||
      content_type == CONTENT_SETTINGS_TYPE_CLIPBOARD_READ ||
      content_type == CONTENT_SETTINGS_TYPE_SENSORS) {
    const auto& it = content_settings_status_.find(content_type);
    if (it != content_settings_status_.end())
      return it->second.blocked;
  }

  return false;
}

bool TabSpecificContentSettings::IsContentAllowed(
    ContentSettingsType content_type) const {
  DCHECK_NE(CONTENT_SETTINGS_TYPE_AUTOMATIC_DOWNLOADS, content_type)
      << "Automatic downloads handled by DownloadRequestLimiter";

  // This method currently only returns meaningful values for the content type
  // cookies, media, PPAPI broker, downloads, MIDI sysex, clipboard, and
  // sensors.
  if (content_type != CONTENT_SETTINGS_TYPE_COOKIES &&
      content_type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC &&
      content_type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA &&
      content_type != CONTENT_SETTINGS_TYPE_PPAPI_BROKER &&
      content_type != CONTENT_SETTINGS_TYPE_MIDI_SYSEX &&
      content_type != CONTENT_SETTINGS_TYPE_CLIPBOARD_READ &&
      content_type != CONTENT_SETTINGS_TYPE_SENSORS) {
    return false;
  }

  const auto& it = content_settings_status_.find(content_type);
  if (it != content_settings_status_.end())
    return it->second.allowed;
  return false;
}

void TabSpecificContentSettings::OnContentBlocked(ContentSettingsType type) {
  OnContentBlockedWithDetail(type, base::string16());
}

void TabSpecificContentSettings::OnContentBlockedWithDetail(
    ContentSettingsType type,
    const base::string16& details) {
  DCHECK(type != CONTENT_SETTINGS_TYPE_GEOLOCATION)
      << "Geolocation settings handled by OnGeolocationPermissionSet";
  DCHECK(type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC &&
         type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA)
      << "Media stream settings handled by OnMediaStreamPermissionSet";
  if (!content_settings::ContentSettingsRegistry::GetInstance()->Get(type))
    return;

#if BUILDFLAG(ENABLE_EXTENSIONS)
  guest_view::GuestViewBase* guest =
      guest_view::GuestViewBase::FromWebContents(web_contents());

  if (guest) {
    static_cast<extensions::WebViewGuest*>(guest)->OnContentBlocked(type,
                                                                    details);
  }
#endif

  ContentSettingsStatus& status = content_settings_status_[type];

  if (!status.blocked) {
    status.blocked = true;
    content_settings::UpdateLocationBarUiForWebContents(web_contents());

    if (type == CONTENT_SETTINGS_TYPE_MIXEDSCRIPT) {
      content_settings::RecordMixedScriptAction(
          content_settings::MIXED_SCRIPT_ACTION_DISPLAYED_SHIELD);
    } else if (type == CONTENT_SETTINGS_TYPE_PLUGINS) {
      content_settings::RecordPluginsAction(
          content_settings::PLUGINS_ACTION_DISPLAYED_BLOCKED_ICON_IN_OMNIBOX);
    } else if (type == CONTENT_SETTINGS_TYPE_POPUPS) {
      content_settings::RecordPopupsAction(
          content_settings::POPUPS_ACTION_DISPLAYED_BLOCKED_ICON_IN_OMNIBOX);
    }
  }
}

void TabSpecificContentSettings::OnContentAllowed(ContentSettingsType type) {
  DCHECK(type != CONTENT_SETTINGS_TYPE_GEOLOCATION)
      << "Geolocation settings handled by OnGeolocationPermissionSet";
  DCHECK(type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC &&
         type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA)
      << "Media stream settings handled by OnMediaStreamPermissionSet";
  bool access_changed = false;
  ContentSettingsStatus& status = content_settings_status_[type];

  // Whether to reset status for the |blocked| setting to avoid ending up
  // with both |allowed| and |blocked| set, which can mean multiple things
  // (allowed setting that got disabled, disabled setting that got enabled).
  bool must_reset_blocked_status = false;

  // For sensors, the status with both allowed/blocked flags set means that
  // access was previously allowed but the last decision was to block.
  // Reset the blocked flag so that the UI will properly indicate that the
  // last decision here instead was to allow sensor access.
  if (type == CONTENT_SETTINGS_TYPE_SENSORS)
    must_reset_blocked_status = true;

#if defined(OS_ANDROID)
  // content_settings_status_[type].allowed is always set to true in
  // OnContentBlocked, so we have to use
  // content_settings_status_[type].blocked to detect whether the protected
  // media setting has changed.
  if (type == CONTENT_SETTINGS_TYPE_PROTECTED_MEDIA_IDENTIFIER)
    must_reset_blocked_status = true;
#endif

  if (must_reset_blocked_status && status.blocked) {
    status.blocked = false;
    access_changed = true;
  }

  if (!status.allowed) {
    status.allowed = true;
    access_changed = true;
  }

  if (access_changed)
    content_settings::UpdateLocationBarUiForWebContents(web_contents());
}

void TabSpecificContentSettings::OnCookiesRead(
    const GURL& url,
    const GURL& frame_url,
    const net::CookieList& cookie_list,
    bool blocked_by_policy) {
  if (cookie_list.empty())
    return;
  if (blocked_by_policy) {
    blocked_local_shared_objects_.cookies()->AddReadCookies(
        frame_url, url, cookie_list);
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.cookies()->AddReadCookies(
        frame_url, url, cookie_list);
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnCookieChange(
    const GURL& url,
    const GURL& frame_url,
    const net::CanonicalCookie& cookie,
    bool blocked_by_policy) {
  if (blocked_by_policy) {
    blocked_local_shared_objects_.cookies()->AddChangedCookie(frame_url, url,
                                                              cookie);
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.cookies()->AddChangedCookie(frame_url, url,
                                                              cookie);
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnIndexedDBAccessed(const GURL& url,
                                                     bool blocked_by_policy) {
  if (blocked_by_policy) {
    blocked_local_shared_objects_.indexed_dbs()->Add(url::Origin::Create(url));
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.indexed_dbs()->Add(url::Origin::Create(url));
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnCacheStorageAccessed(
    const GURL& url,
    bool blocked_by_policy) {
  if (blocked_by_policy) {
    blocked_local_shared_objects_.cache_storages()->Add(
        url::Origin::Create(url));
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.cache_storages()->Add(
        url::Origin::Create(url));
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnLocalStorageAccessed(
    const GURL& url,
    bool local,
    bool blocked_by_policy) {
  LocalSharedObjectsContainer& container = blocked_by_policy ?
      blocked_local_shared_objects_ : allowed_local_shared_objects_;
  CannedBrowsingDataLocalStorageHelper* helper =
      local ? container.local_storages() : container.session_storages();
  helper->Add(url::Origin::Create(url));

  if (blocked_by_policy)
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  else
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnServiceWorkerAccessed(
    const GURL& scope,
    bool blocked_by_policy_javascript,
    bool blocked_by_policy_cookie) {
  DCHECK(scope.is_valid());
  if (blocked_by_policy_javascript || blocked_by_policy_cookie) {
    blocked_local_shared_objects_.service_workers()->Add(
        url::Origin::Create(scope));
  } else {
    allowed_local_shared_objects_.service_workers()->Add(
        url::Origin::Create(scope));
  }

  if (blocked_by_policy_javascript) {
    OnContentBlocked(CONTENT_SETTINGS_TYPE_JAVASCRIPT);
  } else {
    OnContentAllowed(CONTENT_SETTINGS_TYPE_JAVASCRIPT);
  }
  if (blocked_by_policy_cookie) {
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }
}

void TabSpecificContentSettings::OnSharedWorkerAccessed(
    const GURL& worker_url,
    const std::string& name,
    const url::Origin& constructor_origin,
    bool blocked_by_policy) {
  DCHECK(worker_url.is_valid());
  if (blocked_by_policy) {
    blocked_local_shared_objects_.shared_workers()->AddSharedWorker(
        worker_url, name, constructor_origin);
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.shared_workers()->AddSharedWorker(
        worker_url, name, constructor_origin);
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }
}

void TabSpecificContentSettings::OnWebDatabaseAccessed(
    const GURL& url,
    bool blocked_by_policy) {
  if (blocked_by_policy) {
    blocked_local_shared_objects_.databases()->Add(url::Origin::Create(url));
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.databases()->Add(url::Origin::Create(url));
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnFileSystemAccessed(
    const GURL& url,
    bool blocked_by_policy) {
  // Note that all sandboxed file system access is recorded here as
  // kTemporary; the distinction between temporary (default) and persistent
  // storage is not made in the UI that presents this data.
  if (blocked_by_policy) {
    blocked_local_shared_objects_.file_systems()->Add(url::Origin::Create(url));
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.file_systems()->Add(url::Origin::Create(url));
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }

  NotifySiteDataObservers();
}

void TabSpecificContentSettings::OnGeolocationPermissionSet(
    const GURL& requesting_origin,
    bool allowed) {
  geolocation_usages_state_.OnPermissionSet(requesting_origin, allowed);
  content_settings::UpdateLocationBarUiForWebContents(web_contents());
}

#if defined(OS_ANDROID) || defined(OS_CHROMEOS)
void TabSpecificContentSettings::OnProtectedMediaIdentifierPermissionSet(
    const GURL& requesting_origin,
    bool allowed) {
  if (allowed) {
    OnContentAllowed(CONTENT_SETTINGS_TYPE_PROTECTED_MEDIA_IDENTIFIER);
  } else {
    OnContentBlocked(CONTENT_SETTINGS_TYPE_PROTECTED_MEDIA_IDENTIFIER);
  }
}
#endif

TabSpecificContentSettings::MicrophoneCameraState
TabSpecificContentSettings::GetMicrophoneCameraState() const {
  MicrophoneCameraState state = microphone_camera_state_;

  // Include capture devices in the state if there are still consumers of the
  // approved media stream.
  scoped_refptr<MediaStreamCaptureIndicator> media_indicator =
      MediaCaptureDevicesDispatcher::GetInstance()->
        GetMediaStreamCaptureIndicator();
  if (media_indicator->IsCapturingAudio(web_contents()))
    state |= MICROPHONE_ACCESSED;
  if (media_indicator->IsCapturingVideo(web_contents()))
    state |= CAMERA_ACCESSED;

  return state;
}

bool TabSpecificContentSettings::IsMicrophoneCameraStateChanged() const {
  if ((microphone_camera_state_ & MICROPHONE_ACCESSED) &&
      ((microphone_camera_state_& MICROPHONE_BLOCKED) ?
        !IsContentBlocked(CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC) :
        !IsContentAllowed(CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC)))
    return true;

  if ((microphone_camera_state_ & CAMERA_ACCESSED) &&
      ((microphone_camera_state_ & CAMERA_BLOCKED) ?
        !IsContentBlocked(CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA) :
        !IsContentAllowed(CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA)))
    return true;

  PrefService* prefs =
      Profile::FromBrowserContext(web_contents()->GetBrowserContext())->
          GetPrefs();
  scoped_refptr<MediaStreamCaptureIndicator> media_indicator =
      MediaCaptureDevicesDispatcher::GetInstance()->
          GetMediaStreamCaptureIndicator();

  if ((microphone_camera_state_ & MICROPHONE_ACCESSED) &&
      prefs->GetString(prefs::kDefaultAudioCaptureDevice) !=
      media_stream_selected_audio_device() &&
      media_indicator->IsCapturingAudio(web_contents()))
    return true;

  if ((microphone_camera_state_ & CAMERA_ACCESSED) &&
      prefs->GetString(prefs::kDefaultVideoCaptureDevice) !=
      media_stream_selected_video_device() &&
      media_indicator->IsCapturingVideo(web_contents()))
    return true;

  return false;
}

void TabSpecificContentSettings::OnMediaStreamPermissionSet(
    const GURL& request_origin,
    MicrophoneCameraState new_microphone_camera_state,
    const std::string& media_stream_selected_audio_device,
    const std::string& media_stream_selected_video_device,
    const std::string& media_stream_requested_audio_device,
    const std::string& media_stream_requested_video_device) {
  media_stream_access_origin_ = request_origin;

  if (new_microphone_camera_state & MICROPHONE_ACCESSED) {
    media_stream_requested_audio_device_ = media_stream_requested_audio_device;
    media_stream_selected_audio_device_ = media_stream_selected_audio_device;
    bool mic_blocked = (new_microphone_camera_state & MICROPHONE_BLOCKED) != 0;
    ContentSettingsStatus& status =
        content_settings_status_[CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC];
    status.allowed = !mic_blocked;
    status.blocked = mic_blocked;
  }

  if (new_microphone_camera_state & CAMERA_ACCESSED) {
    media_stream_requested_video_device_ = media_stream_requested_video_device;
    media_stream_selected_video_device_ = media_stream_selected_video_device;
    bool cam_blocked = (new_microphone_camera_state & CAMERA_BLOCKED) != 0;
    ContentSettingsStatus& status =
        content_settings_status_[CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA];
    status.allowed = !cam_blocked;
    status.blocked = cam_blocked;
  }

  if (microphone_camera_state_ != new_microphone_camera_state) {
    microphone_camera_state_ = new_microphone_camera_state;
    content_settings::UpdateLocationBarUiForWebContents(web_contents());
  }
}

void TabSpecificContentSettings::OnMidiSysExAccessed(
    const GURL& requesting_origin) {
  midi_usages_state_.OnPermissionSet(requesting_origin, true);
  OnContentAllowed(CONTENT_SETTINGS_TYPE_MIDI_SYSEX);
}

void TabSpecificContentSettings::OnMidiSysExAccessBlocked(
    const GURL& requesting_origin) {
  midi_usages_state_.OnPermissionSet(requesting_origin, false);
  OnContentBlocked(CONTENT_SETTINGS_TYPE_MIDI_SYSEX);
}

void TabSpecificContentSettings::
ClearContentSettingsExceptForNavigationRelatedSettings() {
  for (auto& status : content_settings_status_) {
    if (status.first == CONTENT_SETTINGS_TYPE_COOKIES ||
        status.first == CONTENT_SETTINGS_TYPE_JAVASCRIPT)
      continue;
    status.second.blocked = false;
    status.second.allowed = false;
  }
  microphone_camera_state_ = MICROPHONE_CAMERA_NOT_ACCESSED;
  load_plugins_link_enabled_ = true;
  content_settings::UpdateLocationBarUiForWebContents(web_contents());
}

void TabSpecificContentSettings::ClearNavigationRelatedContentSettings() {
  blocked_local_shared_objects_.Reset();
  allowed_local_shared_objects_.Reset();
  for (ContentSettingsType type :
    {CONTENT_SETTINGS_TYPE_COOKIES, CONTENT_SETTINGS_TYPE_JAVASCRIPT}) {
    ContentSettingsStatus& status =
        content_settings_status_[type];
    status.blocked = false;
    status.allowed = false;
  }
  content_settings::UpdateLocationBarUiForWebContents(web_contents());
}

void TabSpecificContentSettings::FlashDownloadBlocked() {
  OnContentBlockedWithDetail(CONTENT_SETTINGS_TYPE_PLUGINS,
                             base::UTF8ToUTF16(content::kFlashPluginName));
}

void TabSpecificContentSettings::ClearPopupsBlocked() {
  ContentSettingsStatus& status =
      content_settings_status_[CONTENT_SETTINGS_TYPE_POPUPS];
  status.blocked = false;
  content_settings::UpdateLocationBarUiForWebContents(web_contents());
}

void TabSpecificContentSettings::OnAudioBlocked() {
  OnContentBlocked(CONTENT_SETTINGS_TYPE_SOUND);
}

void TabSpecificContentSettings::SetPepperBrokerAllowed(bool allowed) {
  if (allowed) {
    OnContentAllowed(CONTENT_SETTINGS_TYPE_PPAPI_BROKER);
  } else {
    OnContentBlocked(CONTENT_SETTINGS_TYPE_PPAPI_BROKER);
  }
}

void TabSpecificContentSettings::OnContentSettingChanged(
    const ContentSettingsPattern& primary_pattern,
    const ContentSettingsPattern& secondary_pattern,
    ContentSettingsType content_type,
    const std::string& resource_identifier) {
  const ContentSettingsDetails details(
      primary_pattern, secondary_pattern, content_type, resource_identifier);
  if (!details.update_all() &&
      // The visible URL is the URL in the URL field of a tab.
      // Currently this should be matched by the |primary_pattern|.
      !details.primary_pattern().Matches(web_contents()->GetVisibleURL())) {
    return;
  }

  if (content_type == CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC ||
      content_type == CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA) {
    const GURL media_origin = media_stream_access_origin();
    ContentSetting setting = map_->GetContentSetting(
        media_origin, media_origin, content_type, std::string());
    ContentSettingsStatus& status = content_settings_status_[content_type];
    status.allowed = setting == CONTENT_SETTING_ALLOW;
    status.blocked = setting == CONTENT_SETTING_BLOCK;
  }

  if (!ShouldSendUpdatedContentSettingsRulesToRenderer(content_type))
    return;

  MaybeSendRendererContentSettingsRules(web_contents());
}

void TabSpecificContentSettings::MaybeSendRendererContentSettingsRules(
    content::WebContents* web_contents) {
  // Only send a message to the renderer if it is initialised and not dead.
  // Otherwise, the IPC messages will be queued in the browser process,
  // potentially causing large memory leaks. See https://crbug.com/875937.
  content::RenderProcessHost* process =
      web_contents->GetMainFrame()->GetProcess();
  if (!process->IsInitializedAndNotDead())
    return;

  // |channel| may be null in tests.
  IPC::ChannelProxy* channel = process->GetChannel();
  if (!channel)
    return;

  RendererContentSettingRules rules;
  GetRendererContentSettingRules(map_, &rules);

  chrome::mojom::RendererConfigurationAssociatedPtr rc_interface;
  channel->GetRemoteAssociatedInterface(&rc_interface);
  rc_interface->SetContentSettingRules(rules);
}

void TabSpecificContentSettings::RenderFrameForInterstitialPageCreated(
    content::RenderFrameHost* render_frame_host) {
  // We want to tell the renderer-side code to ignore content settings for this
  // page.
  chrome::mojom::ContentSettingsRendererAssociatedPtr content_settings_renderer;
  render_frame_host->GetRemoteAssociatedInterfaces()->GetInterface(
      &content_settings_renderer);
  content_settings_renderer->SetAsInterstitial();
}

bool TabSpecificContentSettings::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* render_frame_host) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(TabSpecificContentSettings, message)
    IPC_MESSAGE_HANDLER(ChromeViewHostMsg_ContentBlocked,
                        OnContentBlockedWithDetail)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void TabSpecificContentSettings::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInMainFrame() ||
      navigation_handle->IsSameDocument()) {
    return;
  }

  // If we're displaying a network error page do not reset the content
  // settings delegate's cookies so the user has a chance to modify cookie
  // settings.
  if (!navigation_handle->IsErrorPage())
    ClearNavigationRelatedContentSettings();
  ClearGeolocationContentSettings();
  ClearMidiContentSettings();
  ClearPendingProtocolHandler();
  ClearContentSettingsChangedViaPageInfo();
}

void TabSpecificContentSettings::ReadyToCommitNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInMainFrame() ||
      navigation_handle->IsSameDocument()) {
    return;
  }

  // There may be content settings that were updated for the navigated URL.
  // These would not have been sent before if we're navigating cross-origin.
  // Ensure up to date rules are sent before navigation commits.
  MaybeSendRendererContentSettingsRules(navigation_handle->GetWebContents());
}

void TabSpecificContentSettings::DidFinishNavigation(
      content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInMainFrame() ||
      !navigation_handle->HasCommitted() ||
      navigation_handle->IsSameDocument()) {
    return;
  }

  // Clear "blocked" flags.
  ClearContentSettingsExceptForNavigationRelatedSettings();
  GeolocationDidNavigate(navigation_handle);
  MidiDidNavigate(navigation_handle);

  if (web_contents()->GetVisibleURL().SchemeIsHTTPOrHTTPS()) {
    content_settings::RecordPluginsAction(
        content_settings::PLUGINS_ACTION_TOTAL_NAVIGATIONS);
  }
}

void TabSpecificContentSettings::AppCacheAccessed(const GURL& manifest_url,
                                                  bool blocked_by_policy) {
  if (blocked_by_policy) {
    blocked_local_shared_objects_.appcaches()->Add(
        url::Origin::Create(manifest_url));
    OnContentBlocked(CONTENT_SETTINGS_TYPE_COOKIES);
  } else {
    allowed_local_shared_objects_.appcaches()->Add(
        url::Origin::Create(manifest_url));
    OnContentAllowed(CONTENT_SETTINGS_TYPE_COOKIES);
  }
}

void TabSpecificContentSettings::AddSiteDataObserver(
    SiteDataObserver* observer) {
  observer_list_.AddObserver(observer);
}

void TabSpecificContentSettings::RemoveSiteDataObserver(
    SiteDataObserver* observer) {
  observer_list_.RemoveObserver(observer);
}

void TabSpecificContentSettings::NotifySiteDataObservers() {
  for (SiteDataObserver& observer : observer_list_)
    observer.OnSiteDataAccessed();
}

void TabSpecificContentSettings::ClearGeolocationContentSettings() {
  geolocation_usages_state_.ClearStateMap();
}

void TabSpecificContentSettings::ClearMidiContentSettings() {
  midi_usages_state_.ClearStateMap();
}

void TabSpecificContentSettings::ClearContentSettingsChangedViaPageInfo() {
  content_settings_changed_via_page_info_.clear();
}

void TabSpecificContentSettings::GeolocationDidNavigate(
    content::NavigationHandle* navigation_handle) {
  geolocation_usages_state_.DidNavigate(navigation_handle->GetURL(),
                                        navigation_handle->GetPreviousURL());
}

void TabSpecificContentSettings::MidiDidNavigate(
    content::NavigationHandle* navigation_handle) {
  midi_usages_state_.DidNavigate(navigation_handle->GetURL(),
                                 navigation_handle->GetPreviousURL());
}

void TabSpecificContentSettings::BlockAllContentForTesting() {
  content_settings::ContentSettingsRegistry* registry =
      content_settings::ContentSettingsRegistry::GetInstance();
  for (const content_settings::ContentSettingsInfo* info : *registry) {
    ContentSettingsType type = info->website_settings_info()->type();
    if (type != CONTENT_SETTINGS_TYPE_GEOLOCATION &&
        type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC &&
        type != CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA) {
      OnContentBlocked(type);
    }
  }

  // Geolocation and media must be blocked separately, as the generic
  // TabSpecificContentSettings::OnContentBlocked does not apply to them.
  OnGeolocationPermissionSet(web_contents()->GetLastCommittedURL(), false);
  MicrophoneCameraStateFlags media_blocked =
      static_cast<MicrophoneCameraStateFlags>(
          TabSpecificContentSettings::MICROPHONE_ACCESSED |
          TabSpecificContentSettings::MICROPHONE_BLOCKED |
          TabSpecificContentSettings::CAMERA_ACCESSED |
          TabSpecificContentSettings::CAMERA_BLOCKED);
  OnMediaStreamPermissionSet(
      web_contents()->GetLastCommittedURL(),
      media_blocked,
      std::string(), std::string(), std::string(), std::string());
}

void TabSpecificContentSettings::ContentSettingChangedViaPageInfo(
    ContentSettingsType type) {
  content_settings_changed_via_page_info_.insert(type);
}

bool TabSpecificContentSettings::HasContentSettingChangedViaPageInfo(
    ContentSettingsType type) const {
  return content_settings_changed_via_page_info_.find(type) !=
         content_settings_changed_via_page_info_.end();
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(TabSpecificContentSettings)
