// Copyright (c) 2017 Vivaldi Technologies AS. All rights reserved.
//
// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/vivaldi_browser_window.h"

#include <memory>
#include <string>
#include <utility>

#include "app/vivaldi_constants.h"
#include "base/command_line.h"
#include "base/json/json_reader.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "browser/menus/vivaldi_menus.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/devtools/devtools_contents_resizing_strategy.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/extensions/window_controller.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/password_manager/chrome_password_manager_client.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/renderer_preferences_util.h"
#include "chrome/browser/sessions/session_tab_helper.h"
#include "chrome/browser/ui/apps/chrome_app_delegate.h"
#include "chrome/browser/ui/autofill/chrome_autofill_client.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_dialogs.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/browser_window_state.h"
#include "chrome/browser/ui/color_chooser.h"
#include "chrome/browser/ui/find_bar/find_bar.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "chrome/browser/ui/tab_contents/core_tab_helper.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/download/download_in_progress_dialog_view.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_view.h"
#include "chrome/browser/ui/views/sharing/click_to_call/click_to_call_dialog_view.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "components/printing/browser/print_composite_client.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager_delegate.h"
#include "content/browser/browser_plugin/browser_plugin_guest.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/site_instance.h"
#include "extensions/api/tabs/tabs_private_api.h"
#include "extensions/api/vivaldi_utilities/vivaldi_utilities_api.h"
#include "extensions/browser/app_window/native_app_window.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_web_contents_observer.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "extensions/browser/view_type_utils.h"
#include "extensions/common/extension_messages.h"
#include "extensions/helper/vivaldi_app_helper.h"
#include "extensions/schema/window_private.h"
#include "extensions/tools/vivaldi_tools.h"
#include "renderer/vivaldi_render_messages.h"
#include "ui/devtools/devtools_connector.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/vivaldi_app_window_client.h"
#include "ui/vivaldi_ui_utils.h"
#include "vivaldi/prefs/vivaldi_gen_prefs.h"

#if defined(OS_WIN)
#include "chrome/browser/win/jumplist_factory.h"
#endif

using extensions::AppWindow;
using extensions::NativeAppWindow;
using web_modal::WebContentsModalDialogManager;

namespace {
base::TimeTicks g_first_window_creation_time;
}  // namespace

namespace extensions {

VivaldiAppWindowContentsImpl::VivaldiAppWindowContentsImpl(
    VivaldiBrowserWindow* host,
    VivaldiWindowEventDelegate* delegate,
    AppDelegate* app_delegate)
    : host_(host),
      delegate_(delegate),
      load_timeout_(),
      app_delegate_(app_delegate) {}

VivaldiAppWindowContentsImpl::~VivaldiAppWindowContentsImpl() {}

void VivaldiAppWindowContentsImpl::Initialize(
    content::BrowserContext* context,
    content::RenderFrameHost* creator_frame,
    const GURL& url) {
  url_ = url;

  scoped_refptr<content::SiteInstance> site_instance =
      content::SiteInstance::CreateForURL(context, url);

  content::RenderProcessHost* process = site_instance->GetProcess();
  content::WebContents::CreateParams create_params(context,
                                                   site_instance.get());
  create_params.opener_render_process_id = process->GetID();
  create_params.opener_render_frame_id =
      creator_frame ? creator_frame->GetRoutingID() : 0;

  web_contents_ = content::WebContents::Create(create_params);

  blink::mojom::RendererPreferences* render_prefs =
      web_contents_->GetMutableRendererPrefs();
  DCHECK(render_prefs);

  // We must update from system settings otherwise many settings would fallback
  // to default values when syncing below.  Guest views use these values from
  // the owner as default values in BrowserPluginGuest::InitInternal().
  Profile* profile = Profile::FromBrowserContext(context);
  renderer_preferences_util::UpdateFromSystemSettings(render_prefs, profile);

  WebContentsObserver::Observe(web_contents_.get());
  web_contents_->GetMutableRendererPrefs()
      ->browser_handles_all_top_level_requests = true;
  web_contents_->GetRenderViewHost()->SyncRendererPrefs();
  web_contents_->SetDelegate(this);

  TabsPrivateAPI::SetupWebContents(web_contents_.get());

  helper_.reset(new AppWebContentsHelper(context, host_->extension()->id(),
                                         web_contents_.get(), app_delegate_));
}

void VivaldiAppWindowContentsImpl::LoadContents(int32_t creator_process_id) {
  // Sandboxed page that are not in the Chrome App package are loaded in a
  // different process.
  if (web_contents_->GetMainFrame()->GetProcess()->GetID() !=
      creator_process_id) {
    VLOG(1) << "AppWindow created in new process ("
            << web_contents_->GetMainFrame()->GetProcess()->GetID()
            << ") != creator (" << creator_process_id << "). Routing disabled.";
  }
  web_contents_->GetController().LoadURL(
      url_, content::Referrer(), ui::PAGE_TRANSITION_LINK, std::string());

  // Ensure we force show the window after some time no matter what.
  load_timeout_.Start(FROM_HERE, base::TimeDelta::FromMilliseconds(5000),
                      base::Bind(&VivaldiAppWindowContentsImpl::ForceShowWindow,
                                 base::Unretained(this)));
}

void VivaldiAppWindowContentsImpl::NativeWindowChanged(
    NativeAppWindow* native_app_window) {
  StateData old_state = state_;
  StateData new_state;
  new_state.is_fullscreen_ = native_app_window->IsFullscreenOrPending();
  new_state.is_minimized_ = native_app_window->IsMinimized();
  new_state.is_maximized_ = native_app_window->IsMaximized();
  new_state.bounds_ = native_app_window->GetBounds();

  // Call the delegate so it can dispatch events to the js side
  // for any change in state.
  if (old_state.is_fullscreen_ != new_state.is_fullscreen_)
    delegate_->OnFullscreenChanged(new_state.is_fullscreen_);

  if (old_state.is_maximized_ != new_state.is_maximized_)
    delegate_->OnMaximizedChanged(new_state.is_maximized_);

  if (old_state.is_minimized_ != new_state.is_minimized_)
    delegate_->OnMinimizedChanged(new_state.is_minimized_);

  if (old_state.bounds_.x() != new_state.bounds_.x() ||
      old_state.bounds_.y() != new_state.bounds_.y()) {
    // We only send an event when the position of the window changes.
    delegate_->OnPositionChanged();
  }

  state_ = new_state;
}

void VivaldiAppWindowContentsImpl::NativeWindowClosed(bool send_onclosed) {
  load_timeout_.Stop();
  web_contents_.reset(nullptr);
}

content::WebContents* VivaldiAppWindowContentsImpl::GetWebContents() const {
  return web_contents_.get();
}

WindowController* VivaldiAppWindowContentsImpl::GetWindowController() const {
  return nullptr;
}

bool VivaldiAppWindowContentsImpl::HandleKeyboardEvent(
    content::WebContents* source,
    const content::NativeWebKeyboardEvent& event) {
  return host_->HandleKeyboardEvent(event);
}

bool VivaldiAppWindowContentsImpl::PreHandleGestureEvent(
    content::WebContents* source,
    const blink::WebGestureEvent& event) {
  // When called this means the user has attempted a gesture on the UI. We do
  // not allow that.
#ifdef OS_MACOSX
  if (event.GetType() == blink::WebInputEvent::kGestureDoubleTap)
    return true;
#endif
  return blink::WebInputEvent::IsPinchGestureEventType(event.GetType());
}

content::ColorChooser* VivaldiAppWindowContentsImpl::OpenColorChooser(
    content::WebContents* web_contents,
    SkColor initial_color,
    const std::vector<blink::mojom::ColorSuggestionPtr>& suggestions) {
  return chrome::ShowColorChooser(web_contents, initial_color);
}

void VivaldiAppWindowContentsImpl::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    std::unique_ptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  app_delegate_->RunFileChooser(render_frame_host, std::move(listener), params);
}

void VivaldiAppWindowContentsImpl::NavigationStateChanged(
    content::WebContents* source,
    content::InvalidateTypes changed_flags) {
  if (changed_flags &
      (content::INVALIDATE_TYPE_TAB | content::INVALIDATE_TYPE_TITLE)) {
    host_->UpdateTitleBar();
  }
}

void VivaldiAppWindowContentsImpl::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  DCHECK_EQ(web_contents_.get(), web_contents);
  helper_->RequestMediaAccessPermission(request, std::move(callback));
}

bool VivaldiAppWindowContentsImpl::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const GURL& security_origin,
    blink::mojom::MediaStreamType type) {
  return helper_->CheckMediaAccessPermission(render_frame_host, security_origin, type);
}

// If we should ever need to play PIP videos in our UI, this code enables
// it. The implementation for webpages is in WebViewGuest.
content::PictureInPictureResult VivaldiAppWindowContentsImpl::EnterPictureInPicture(
    WebContents* web_contents,
    const viz::SurfaceId& surface_id,
    const gfx::Size& natural_size) {
  return app_delegate_->EnterPictureInPicture(web_contents, surface_id,
                                              natural_size);
}

void VivaldiAppWindowContentsImpl::ExitPictureInPicture() {
  app_delegate_->ExitPictureInPicture();
}

void VivaldiAppWindowContentsImpl::PrintCrossProcessSubframe(
    WebContents* web_contents,
    const gfx::Rect& rect,
    int document_cookie,
    content::RenderFrameHost* subframe_host) const {
  auto* client = printing::PrintCompositeClient::FromWebContents(web_contents);
  if (client) {
    client->PrintCrossProcessSubframe(rect, document_cookie, subframe_host);
  }
}

void VivaldiAppWindowContentsImpl::ActivateContents(WebContents* contents) {
  host_->Activate();
}

void VivaldiAppWindowContentsImpl::RenderViewCreated(
    content::RenderViewHost* render_view_host) {
  // An incognito profile is not initialized with the UI zoom value. Set it up
  // here by reading prefs from the regular profile. At this point we do not
  // know the partition key (see ChromeZoomLevelPrefs::InitHostZoomMap) so we
  // just test all keys until we match the kVivaldiAppId host.
  if (host_->GetProfile()->IsOffTheRecord()) {
    PrefService* pref_service =
        host_->GetProfile()->GetOriginalProfile()->GetPrefs();
    const base::DictionaryValue* partition_dict =
        pref_service->GetDictionary(prefs::kPartitionPerHostZoomLevels);
    bool match = false;
    for (base::DictionaryValue::Iterator partition(*partition_dict);
         !partition.IsAtEnd() && !match; partition.Advance()) {
      const base::DictionaryValue* host_dict = nullptr;
      if (partition_dict->GetDictionary(partition.key(), &host_dict)) {
        // Test each host until we match kVivaldiAppId
        for (base::DictionaryValue::Iterator host(*host_dict); !host.IsAtEnd();
             host.Advance()) {
          if (host.key() == ::vivaldi::kVivaldiAppId) {
            match = true;
            // Each host is another dictionary with settings
            const base::DictionaryValue* settings_dict = nullptr;
            if (host.value().GetAsDictionary(&settings_dict)) {
              for (base::DictionaryValue::Iterator setting(*settings_dict);
                  !setting.IsAtEnd(); setting.Advance()) {
                if (setting.key() == "zoom_level") {
                  double zoom_level = 0;
                  if (setting.value().GetAsDouble(&zoom_level)) {
                    content::HostZoomMap* zoom_map =
                        content::HostZoomMap::GetForWebContents(
                            this->GetWebContents());
                    DCHECK(zoom_map);
                    zoom_map->SetZoomLevelForHost(::vivaldi::kVivaldiAppId,
                        zoom_level);
                  }
                  break;
                }
              }
            }
          }
          break;
        }
      }
    }
  }
}

namespace {

void OnUIProcessCrash(base::TerminationStatus status) {
  static bool after_ui_crash = false;
  if (after_ui_crash)
    return;
  after_ui_crash = true;
  double uptime_seconds =
      (base::TimeTicks::Now() - g_first_window_creation_time).InSecondsF();
  LOG(ERROR) << "UI Process adnormally terminates with status " << status
             << " after running for " << uptime_seconds << " seconds!";

  // Restart or exit while preserving the tab and window session as it was
  // before the crash. For that pretend that we got the end-of-ssession signal
  // that makes Chromium to close all windows without running any unload
  // handlers or recording session updates.
  browser_shutdown::OnShutdownStarting(browser_shutdown::END_SESSION);

  chrome::CloseAllBrowsers();

  bool want_restart = false;
#ifndef DEBUG
  // TODO(igor@vivaldi.com): Consider restarting on
  // TERMINATION_STATUS_PROCESS_WAS_KILLED in addition to crashes in case
  // the user accidentally kills the UI process in the task manager.
  using base::TerminationStatus;
  if (status == TerminationStatus::TERMINATION_STATUS_PROCESS_CRASHED) {
    want_restart = true;
  }
#endif  // _DEBUG
  if (want_restart) {
    // Prevent restart loop if UI crashes shortly after the startup.
    constexpr double MIN_UPTIME_TO_RESTART_SECONDS = 60.0;
    if (uptime_seconds >= MIN_UPTIME_TO_RESTART_SECONDS) {
      LOG(ERROR) << "Restarting Vivaldi";
      chrome::AttemptRestart();
      return;
    }
  }
  LOG(ERROR) << "Quiting Vivaldi";
  chrome::AttemptExit();
}

}  // namespace

void VivaldiAppWindowContentsImpl::RenderProcessGone(
    base::TerminationStatus status) {

  if (status !=
      base::TerminationStatus::TERMINATION_STATUS_NORMAL_TERMINATION
      && status != base::TerminationStatus::TERMINATION_STATUS_STILL_RUNNING) {
    OnUIProcessCrash(status);
  }

  if (!host_->browser())
    return;

  TabStripModel* tab_strip = host_->browser()->tab_strip_model();
  if (tab_strip->empty())
    return;

  // TabStripModel owns WebContents for tabs. If the UI process exits and we
  // reach this point due to potential bugs, we must rip of the remaining tabs.
  // Otherwise when content::RenderFrameHostImpl::RenderProcessExited() closes
  // the tabs later without telling TabStripModel we get double free at least in
  // ~TabStripModel().
  LOG(ERROR) << tab_strip->count()
             << " tabs are still alive after Vivaldi UI shuts down with status "
             << status;

  // Make sure the session is saved and clean up the tabstrip.
  host_->browser()->window()->Close();
}

bool VivaldiAppWindowContentsImpl::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* sender) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(VivaldiAppWindowContentsImpl, message,
                                   sender)
    IPC_MESSAGE_HANDLER(ExtensionHostMsg_UpdateDraggableRegions,
                        UpdateDraggableRegions)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void VivaldiAppWindowContentsImpl::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->HasCommitted()) {
    return;
  }

  // ExtensionFrameHelper::ReadyToCommitNavigation() will suspend the parser
  // to avoid a race condition reported in
  // https://bugs.chromium.org/p/chromium/issues/detail?id=822650.
  // We need to resume the parser here as we do not use the app window
  // bindings.
  content::RenderFrameHostImpl* host =
      static_cast<content::RenderFrameHostImpl*>(
          navigation_handle->GetRenderFrameHost());
  DCHECK(host);
  if (host->GetParent() == nullptr) {
    host->Send(new VivaldiFrameHostMsg_ResumeParser(host->routing_id()));
  }
}

void VivaldiAppWindowContentsImpl::UpdateDraggableRegions(
    content::RenderFrameHost* sender,
    const std::vector<DraggableRegion>& regions) {
  // Only process events for the main frame.
  if (!sender->GetParent()) {
    host_->UpdateDraggableRegions(regions);
  }
}

void VivaldiAppWindowContentsImpl::DidFinishLoad(
    content::RenderFrameHost* render_frame_host,
    const GURL& validated_url) {
  host_->UpdateTitleBar();
  delegate_->OnDocumentLoaded();
  load_timeout_.Stop();
}

void VivaldiAppWindowContentsImpl::ForceShowWindow() {
  delegate_->OnDocumentLoaded();
  load_timeout_.Stop();
}

}  // namespace extensions

// VivaldiBrowserWindow --------------------------------------------------------

VivaldiBrowserWindow::VivaldiBrowserWindow() {
  if (g_first_window_creation_time.is_null()) {
    g_first_window_creation_time = base::TimeTicks::Now();
  }
}

VivaldiBrowserWindow::~VivaldiBrowserWindow() {
  // Explicitly set browser_ to NULL.
  browser_.reset();
}

void VivaldiBrowserWindow::Init(std::unique_ptr<Browser> browser,
                                std::string& base_url,
                                extensions::AppWindow::CreateParams& params) {
  browser_ = std::move(browser);
  base_url_ = base_url;
  params_ = params;

  if (browser_) {
    chrome::GetSavedWindowBoundsAndShowState(
        browser_.get(), &params_.content_spec.bounds, &initial_state_);
    params_.state = initial_state_;

    location_bar_.reset(new VivaldiLocationBar(browser_->profile(),
        browser_.get()));

    // No need to delay creating the web contents, we have a browser.
    CreateWebContents(nullptr);
  }
}

void VivaldiBrowserWindow::set_browser(Browser* browser) {
  browser_.reset(browser);

  DCHECK(!location_bar_);
  location_bar_.reset(new VivaldiLocationBar(browser_->profile(), browser));
}

// static
VivaldiBrowserWindow* VivaldiBrowserWindow::GetBrowserWindowForBrowser(
    const Browser* browser) {
  return static_cast<VivaldiBrowserWindow*>(browser->window());
}

// static
extensions::AppWindow::CreateParams
VivaldiBrowserWindow::PrepareWindowParameters(Browser* browser,
                                              std::string& base_url) {
  extensions::AppWindow::CreateParams params;

  base_url = "browser.html";
  params.frame = extensions::AppWindow::FRAME_NONE;
  params.window_spec.minimum_size = gfx::Size(500, 300);

  // if browser is nullptr, we expect the callee to set the defaults.
  if (browser && browser->profile()->GetPrefs()->GetBoolean(
                     vivaldiprefs::kWindowsUseNativeDecoration)) {
    params.frame = extensions::AppWindow::FRAME_CHROME;
  }
  return params;
}

// static
VivaldiBrowserWindow* VivaldiBrowserWindow::CreateVivaldiBrowserWindow(
    std::unique_ptr<Browser> browser,
    std::string& base_url,
    extensions::AppWindow::CreateParams& params) {
  VivaldiBrowserWindow* window = new VivaldiBrowserWindow();
  window->Init(std::move(browser), base_url, params);
  return window;
}

void VivaldiBrowserWindow::UpdateDraggableRegions(
    const std::vector<extensions::DraggableRegion>& regions) {
  native_app_window_->UpdateDraggableRegions(regions);
}

void VivaldiBrowserWindow::CreateWebContents(content::RenderFrameHost* host) {
#if defined(OS_WIN)
  JumpListFactory::GetForProfile(browser_->profile());
#endif
  extension_ = const_cast<extensions::Extension*>(
      extensions::ExtensionRegistry::Get(browser_->profile())
          ->GetExtensionById(vivaldi::kVivaldiAppId,
                             extensions::ExtensionRegistry::EVERYTHING));
  DCHECK(extension_);

  GURL url = extension_->GetResourceURL(base_url_);

  // TODO(pettern): Make a VivaldiAppDelegate, remove patches in
  // ChromeAppDelegate.
  app_delegate_.reset(new ChromeAppDelegate(false));
  app_window_contents_.reset(new extensions::VivaldiAppWindowContentsImpl(
      this, this, app_delegate_.get()));

  // We should always be set as vivaldi in Browser.
  DCHECK(browser_->is_vivaldi());

  app_window_contents_->Initialize(browser_->profile(), host, url);

  SetViewType(web_contents(), extensions::VIEW_TYPE_APP_WINDOW);
  app_delegate_->InitWebContents(web_contents());

  extensions::ExtensionWebContentsObserver::GetForWebContents(web_contents())
      ->dispatcher()
      ->set_delegate(this);

  autofill::ChromeAutofillClient::CreateForWebContents(web_contents());
  ChromePasswordManagerClient::CreateForWebContentsWithAutofillClient(
      web_contents(),
      autofill::ChromeAutofillClient::FromWebContents(web_contents()));
  ManagePasswordsUIController::CreateForWebContents(web_contents());
  TabDialogs::CreateForWebContents(web_contents());

  web_modal::WebContentsModalDialogManager::CreateForWebContents(
      web_contents());

  web_modal::WebContentsModalDialogManager::FromWebContents(web_contents())
      ->SetDelegate(this);

  extensions::VivaldiAppHelper::CreateForWebContents(web_contents());
  zoom::ZoomController::CreateForWebContents(web_contents());

  VivaldiAppWindowClient::Set(VivaldiAppWindowClient::GetInstance());

  VivaldiAppWindowClient* app_window_client = VivaldiAppWindowClient::Get();
  native_app_window_.reset(
      app_window_client->CreateNativeAppWindow(this, &params_));

  // The infobar container must come after the toolbar so its arrow paints on
  // top.
  infobar_container_ = new vivaldi::InfoBarContainerWebProxy(this);

  // TODO(pettern): Crashes on shutdown, fix.
  // extensions::ExtensionRegistry::Get(browser_->profile())->AddObserver(this);

  app_window_contents_->LoadContents(params_.creator_process_id);
}

content::WebContents* VivaldiBrowserWindow::web_contents() const {
  if (app_window_contents_)
    return app_window_contents_->GetWebContents();
  return nullptr;
}

void VivaldiBrowserWindow::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const extensions::Extension* extension,
    extensions::UnloadedExtensionReason reason) {
  if (vivaldi::kVivaldiAppId == extension->id())
    native_app_window_->Close();
}

void VivaldiBrowserWindow::Show() {
#if !defined(OS_WIN)
  // The Browser associated with this browser window must become the active
  // browser at the time |Show()| is called. This is the natural behavior under
  // Windows and Ash, but other platforms will not trigger
  // OnWidgetActivationChanged() until we return to the runloop. Therefore any
  // calls to Browser::GetLastActive() will return the wrong result if we do not
  // explicitly set it here.
  // A similar block also appears in BrowserWindowCocoa::Show().
  BrowserList::SetLastActive(browser());
#endif

  // We delay showing it until the UI document has loaded.
  if (!has_loaded_document_)
    return;

  Show(extensions::AppWindow::SHOW_ACTIVE);
}

void VivaldiBrowserWindow::Show(extensions::AppWindow::ShowType show_type) {
  if (has_been_shown_)
    return;
  has_been_shown_ = true;

  app_delegate_->OnShow();
  is_hidden_ = false;

  if (initial_state_ == ui::SHOW_STATE_FULLSCREEN)
    SetFullscreen(true);
  else if (initial_state_ == ui::SHOW_STATE_MAXIMIZED)
    Maximize();
  else if (initial_state_ == ui::SHOW_STATE_MINIMIZED)
    Minimize();

  switch (show_type) {
    case extensions::AppWindow::SHOW_ACTIVE:
      GetBaseWindow()->Show();
      break;
    case extensions::AppWindow::SHOW_INACTIVE:
      GetBaseWindow()->ShowInactive();
      break;
  }

  OnNativeWindowChanged();
}

void VivaldiBrowserWindow::Hide() {
  is_hidden_ = true;
  GetBaseWindow()->Hide();
  app_delegate_->OnHide();
}

bool VivaldiBrowserWindow::IsVisible() const {
  return GetBaseWindow()->IsVisible();
}


void VivaldiBrowserWindow::SetBounds(const gfx::Rect& bounds) {
  if (native_app_window_)
    native_app_window_->SetBounds(bounds);
  return;
}

void VivaldiBrowserWindow::Close() {
  MovePinnedTabsToOtherWindowIfNeeded();

  native_app_window_->Close();
}

void VivaldiBrowserWindow::MovePinnedTabsToOtherWindowIfNeeded() {
  Browser* candidate =
      ::vivaldi::ui_tools::FindBrowserForPinnedTabs(browser_.get());
  if (!candidate) {
    return;
  }
  std::vector<int> tabs_to_move;
  TabStripModel* tab_strip_model = browser_->tab_strip_model();
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    if (tab_strip_model->IsTabPinned(i)) {
      tabs_to_move.push_back(
          SessionTabHelper::IdForTab(tab_strip_model->GetWebContentsAt(i))
              .id());
    }
  }

  // Ensure that all tabs are added after the last pinned tab in the target
  // window.
  int new_index = 0;
  tab_strip_model = candidate->tab_strip_model();
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    if (tab_strip_model->IsTabPinned(i)) {
      new_index = i + 1;
    }
  }

  // We increment the 'new_index' by one ourselves to get all moved pinned tabs
  // alongside to each other.
  int index = 0;
  for (size_t i = 0; i < tabs_to_move.size(); i++) {
    if (::vivaldi::ui_tools::GetTabById(tabs_to_move[i], nullptr, &index)) {
      if (!::vivaldi::ui_tools::MoveTabToWindow(browser_.get(), candidate,
                                                index, &new_index, 0)) {
        NOTREACHED();
        break;
      }
      new_index += 1;
    }
  }
}

void VivaldiBrowserWindow::ConfirmBrowserCloseWithPendingDownloads(
    int download_count,
    Browser::DownloadClosePreventionType dialog_type,
    bool app_modal,
    const base::Callback<void(bool)>& callback) {
#ifdef OS_MACOSX
  // We allow closing the window here since the real quit decision on Mac is
  // made in [AppController quit:].
  callback.Run(true);
#else
  DownloadInProgressDialogView::Show(GetNativeWindow(), download_count,
                                     dialog_type, app_modal, callback);
#endif  // OS_MACOSX
}

void VivaldiBrowserWindow::Activate() {
  native_app_window_->Activate();
  BrowserList::SetLastActive(browser_.get());
}

void VivaldiBrowserWindow::Deactivate() {}

bool VivaldiBrowserWindow::IsActive() const {
  return native_app_window_ ? native_app_window_->IsActive() : false;
}

gfx::NativeWindow VivaldiBrowserWindow::GetNativeWindow() const {
  return GetBaseWindow()->GetNativeWindow();
}

StatusBubble* VivaldiBrowserWindow::GetStatusBubble() {
  return NULL;
}

gfx::Rect VivaldiBrowserWindow::GetRestoredBounds() const {
  if (native_app_window_) {
    return native_app_window_->GetRestoredBounds();
  }
  return gfx::Rect();
}

ui::WindowShowState VivaldiBrowserWindow::GetRestoredState() const {
  if (native_app_window_) {
    return native_app_window_->GetRestoredState();
  }
  return ui::SHOW_STATE_DEFAULT;
}

gfx::Rect VivaldiBrowserWindow::GetBounds() const {
  if (native_app_window_) {
    gfx::Rect bounds = native_app_window_->GetBounds();
    bounds.Inset(native_app_window_->GetFrameInsets());
    return bounds;
  }
  return gfx::Rect();
}

bool VivaldiBrowserWindow::IsMaximized() const {
  if (native_app_window_) {
    return native_app_window_->IsMaximized();
  }
  return false;
}

bool VivaldiBrowserWindow::IsMinimized() const {
  if (native_app_window_) {
    return native_app_window_->IsMinimized();
  }
  return false;
}

void VivaldiBrowserWindow::Maximize() {
  if (native_app_window_) {
    return native_app_window_->Maximize();
  }
}

void VivaldiBrowserWindow::Minimize() {
  if (native_app_window_) {
    return native_app_window_->Minimize();
  }
}

void VivaldiBrowserWindow::Restore() {
  if (IsFullscreen()) {
    native_app_window_->SetFullscreen(
        extensions::AppWindow::FULLSCREEN_TYPE_NONE);
  } else {
    GetBaseWindow()->Restore();
  }
}

bool VivaldiBrowserWindow::ShouldHideUIForFullscreen() const {
  return true;
}

bool VivaldiBrowserWindow::IsFullscreenBubbleVisible() const {
  return false;
}

LocationBar* VivaldiBrowserWindow::GetLocationBar() const {
  return location_bar_.get();
}

void VivaldiBrowserWindow::UpdateToolbar(content::WebContents* contents) {
  GetToolbarPageActionIconContainer()->UpdatePageActionIcon(
      PageActionIconType::kManagePasswords);
}

ToolbarActionsBar* VivaldiBrowserWindow::GetToolbarActionsBar() {
  return NULL;
}

content::KeyboardEventProcessingResult
VivaldiBrowserWindow::PreHandleKeyboardEvent(
    const content::NativeWebKeyboardEvent& event) {
  return content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool VivaldiBrowserWindow::HandleKeyboardEvent(
    const content::NativeWebKeyboardEvent& event) {

  bool is_auto_repeat;
#if defined(OS_MACOSX)
  is_auto_repeat = event.GetModifiers() & blink::WebInputEvent::kIsAutoRepeat;
#else
  // Ideally we should do what we do above for Mac, but it is not 100% reliable
  // (at least on Linux). Try pressing F1 for a while and switch to F2. The
  // first auto repeat is not flagged as such.
  is_auto_repeat = false;
  if (event.GetType() == blink::WebInputEvent::kRawKeyDown) {
    is_auto_repeat = event.windows_key_code == last_key_code_;
    last_key_code_ = event.windows_key_code;
  } else if (event.GetType() != blink::WebInputEvent::kKeyDown &&
             event.GetType() != blink::WebInputEvent::kChar) {
    last_key_code_ = 0;
  }
#endif  // defined(OS_MACOSX)

  extensions::TabsPrivateAPI::SendKeyboardShortcutEvent(browser_->profile(),
                                                        event, is_auto_repeat);

  return native_app_window_->HandleKeyboardEvent(event);
}

bool VivaldiBrowserWindow::GetAcceleratorForCommandId(
    int command_id,
    ui::Accelerator* accelerator) const {
  return vivaldi::VivaldiGetAcceleratorForCommandId(nullptr, command_id,
                                                    accelerator);
}

ui::AcceleratorProvider* VivaldiBrowserWindow::GetAcceleratorProvider() {
  return this;
}

bool VivaldiBrowserWindow::IsBookmarkBarVisible() const {
  return false;
}

bool VivaldiBrowserWindow::IsBookmarkBarAnimating() const {
  return false;
}

bool VivaldiBrowserWindow::IsTabStripEditable() const {
  return true;
}

bool VivaldiBrowserWindow::IsToolbarVisible() const {
  return false;
}

bool VivaldiBrowserWindow::IsDownloadShelfVisible() const {
  return false;
}

DownloadShelf* VivaldiBrowserWindow::GetDownloadShelf() {
  return NULL;
}

// See comments on: BrowserWindow.VivaldiShowWebSiteSettingsAt.
void VivaldiBrowserWindow::VivaldiShowWebsiteSettingsAt(
    Profile* profile,
    content::WebContents* web_contents,
    const GURL& url,
    security_state::SecurityLevel security_level,
    const security_state::VisibleSecurityState& visible_security_state,
    gfx::Point pos) {
#if defined(USE_AURA)
  PageInfoBubbleView::ShowPopupAtPos(pos, profile, web_contents, url,
                                     security_level, visible_security_state,
                                     browser_.get(), GetNativeWindow());
#elif defined(OS_MACOSX)
  gfx::Rect anchor_rect = gfx::Rect(pos, gfx::Size());
  views::BubbleDialogDelegateView* bubble =
      PageInfoBubbleView::CreatePageInfoBubble(
          nullptr, anchor_rect, GetNativeWindow(), profile, web_contents, url,
          security_level, visible_security_state,
          // Use a simple lambda for the callback. We do not do anything there.
          base::BindOnce([](views::Widget::ClosedReason closed_reason,
              bool reload_prompt) {}));
  bubble->GetWidget()->Show();
#endif
}

std::unique_ptr<FindBar> VivaldiBrowserWindow::CreateFindBar() {
  return std::unique_ptr<FindBar>();
}

void VivaldiBrowserWindow::ExecuteExtensionCommand(
    const extensions::Extension* extension,
    const extensions::Command& command) {}

ExclusiveAccessContext* VivaldiBrowserWindow::GetExclusiveAccessContext() {
  return this;
}

autofill::SaveCardBubbleView* VivaldiBrowserWindow::ShowSaveCreditCardBubble(
    content::WebContents* contents,
    autofill::SaveCardBubbleController* controller,
    bool is_user_gesture) {
  return NULL;
}

void VivaldiBrowserWindow::DestroyBrowser() {
  // TODO(pettern): Crashes on shutdown, fix.
  //  extensions::ExtensionRegistry::Get(browser_->profile())->RemoveObserver(this);
  browser_.reset();
}

gfx::Size VivaldiBrowserWindow::GetContentsSize() const {
  if (native_app_window_) {
    // TODO(pettern): This is likely not correct, should be tab contents.
    gfx::Rect bounds = native_app_window_->GetBounds();
    bounds.Inset(native_app_window_->GetFrameInsets());
    return bounds.size();
  }
  return gfx::Size();
}

void VivaldiBrowserWindow::ShowEmojiPanel() {
  native_app_window_->ShowEmojiPanel();
}

std::string VivaldiBrowserWindow::GetWorkspace() const {
  return std::string();
}

bool VivaldiBrowserWindow::IsVisibleOnAllWorkspaces() const {
  return false;
}

Profile* VivaldiBrowserWindow::GetProfile() {
  return browser_->profile();
}

void VivaldiBrowserWindow::UpdateExclusiveAccessExitBubbleContent(
    const GURL& url,
    ExclusiveAccessBubbleType bubble_type,
    ExclusiveAccessBubbleHideCallback bubble_first_hide_callback,
    bool force_update) {}

void VivaldiBrowserWindow::OnExclusiveAccessUserInput() {}

content::WebContents* VivaldiBrowserWindow::GetActiveWebContents() {
  return browser_->tab_strip_model()->GetActiveWebContents();
}

void VivaldiBrowserWindow::UnhideDownloadShelf() {}

void VivaldiBrowserWindow::HideDownloadShelf() {}

ShowTranslateBubbleResult VivaldiBrowserWindow::ShowTranslateBubble(
    content::WebContents* contents,
    translate::TranslateStep step,
    const std::string& source_language,
    const std::string& target_language,
    translate::TranslateErrors::Type error_type,
    bool is_user_gesture) {
  return ShowTranslateBubbleResult::BROWSER_WINDOW_NOT_VALID;
}

void VivaldiBrowserWindow::UpdateDevTools() {
  TabStripModel* tab_strip_model = browser_->tab_strip_model();

  // Get the docking state.
  const base::DictionaryValue* prefs =
      browser_->profile()->GetPrefs()->GetDictionary(
          prefs::kDevToolsPreferences);

  std::string docking_state;
  std::string device_mode;

  // DevToolsWindow code has already activated the tab.
  content::WebContents* contents = tab_strip_model->GetActiveWebContents();
  int tab_id = SessionTabHelper::IdForTab(contents).id();
  extensions::DevtoolsConnectorAPI* api =
      extensions::DevtoolsConnectorAPI::GetFactoryInstance()->Get(
          browser_->profile());
  DCHECK(api);
  scoped_refptr<extensions::DevtoolsConnectorItem> item =
      base::WrapRefCounted<extensions::DevtoolsConnectorItem>(
          api->GetOrCreateDevtoolsConnectorItem(tab_id));

  // Iterate the list of inspected tabs and send events if any is
  // in the process of closing.
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    WebContents* inspected_contents = tab_strip_model->GetWebContentsAt(i);
    DevToolsWindow* w =
        DevToolsWindow::GetInstanceForInspectedWebContents(inspected_contents);
    if (w && w->IsClosing()) {
      int id = SessionTabHelper::IdForTab(inspected_contents).id();
      extensions::DevtoolsConnectorAPI::SendClosed(browser_->profile(), id);
      ResetDockingState(id);
    }
  }
  DevToolsWindow* window =
      DevToolsWindow::GetInstanceForInspectedWebContents(contents);

  if (window) {
    // We handle the closing devtools windows above.
    if (!window->IsClosing()) {
      if (prefs->GetString("currentDockState", &docking_state)) {
        // Strip quotation marks from the state.
        base::ReplaceChars(docking_state, "\"", "", &docking_state);
        if (item->docking_state() != docking_state) {
          item->set_docking_state(docking_state);

          extensions::DevtoolsConnectorAPI::SendDockingStateChanged(
              browser_->profile(), tab_id, docking_state);
        }
      }
      if (prefs->GetString("showDeviceMode", &device_mode)) {
        base::ReplaceChars(device_mode, "\"", "", &device_mode);
        bool device_mode_enabled = device_mode == "true";
        if (item->device_mode_enabled() == device_mode_enabled) {
          item->set_device_mode_enabled(device_mode_enabled);
        }
      }
    }
  }
}

void VivaldiBrowserWindow::ResetDockingState(int tab_id) {
  extensions::DevtoolsConnectorAPI* api =
      extensions::DevtoolsConnectorAPI::GetFactoryInstance()->Get(
          browser_->profile());
  DCHECK(api);
  scoped_refptr<extensions::DevtoolsConnectorItem> item =
       base::WrapRefCounted<extensions::DevtoolsConnectorItem>(
          api->GetOrCreateDevtoolsConnectorItem(tab_id));

  item->ResetDockingState();

  extensions::DevtoolsConnectorAPI::SendDockingStateChanged(
      browser_->profile(), tab_id, item->docking_state());
}

bool VivaldiBrowserWindow::IsToolbarShowing() const {
  return false;
}

ClickToCallDialog* VivaldiBrowserWindow::ShowClickToCallDialog(
    content::WebContents* contents,
    ClickToCallSharingDialogController* controller) {

  auto* dialog_view = new ClickToCallDialogView(
      nullptr/*anchor_view*/, contents, controller);

  views::BubbleDialogDelegateView::CreateBubble(dialog_view)->Show();

  return dialog_view;
}

NativeAppWindow* VivaldiBrowserWindow::GetBaseWindow() const {
  return native_app_window_.get();
}

void VivaldiBrowserWindow::OnNativeWindowChanged(bool moved) {
  // This may be called during Init before |native_app_window_| is set.
  if (!native_app_window_)
    return;

  if (native_app_window_) {
    native_app_window_->UpdateEventTargeterWithInset();
  }

  if (app_window_contents_)
    app_window_contents_->NativeWindowChanged(native_app_window_.get());
}

void VivaldiBrowserWindow::OnNativeClose() {
  WebContentsModalDialogManager* modal_dialog_manager =
      WebContentsModalDialogManager::FromWebContents(web_contents());
  if (modal_dialog_manager) {
    modal_dialog_manager->SetDelegate(nullptr);
  }
  if (app_window_contents_) {
    app_window_contents_->NativeWindowClosed(false);
  }

  // For a while we used a direct "delete this" here. That causes the browser_
  // object to be destoyed immediately and that will kill the private profile.
  // The missing profile will (very often) trigger a crash on Linux. VB-34358
  // TODO(all): There is missing cleanup in chromium code. See VB-34358.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&VivaldiBrowserWindow::DeleteThis, base::Unretained(this)));
}

void VivaldiBrowserWindow::DeleteThis() {
  delete this;
}

void VivaldiBrowserWindow::OnNativeWindowActivationChanged(bool active) {
  UpdateActivation(active);
  if (active)
    BrowserList::SetLastActive(browser_.get());
}

void VivaldiBrowserWindow::UpdateActivation(bool is_active) {
  if (is_active_ != is_active) {
    is_active_ = is_active;
    OnActivationChanged(is_active_);
  }
}

void VivaldiBrowserWindow::UpdateTitleBar() {
  native_app_window_->UpdateWindowTitle();
  native_app_window_->UpdateWindowIcon();
}

base::string16 VivaldiBrowserWindow::GetTitle() {
  if (!extension_)
    return base::string16();

  // WebContents::GetTitle() will return the page's URL if there's no <title>
  // specified. However, we'd prefer to show the name of the extension in that
  // case, so we directly inspect the NavigationEntry's title.
  base::string16 title;
  content::NavigationEntry* entry =
      web_contents() ? web_contents()->GetController().GetLastCommittedEntry()
                     : nullptr;
  if (!entry || entry->GetTitle().empty()) {
    title = base::UTF8ToUTF16(extension_->name());
  } else {
    title = web_contents()->GetTitle();
  }
  title += base::UTF8ToUTF16(" - Vivaldi");
  base::RemoveChars(title, base::ASCIIToUTF16("\n"), &title);
  return title;
}

void VivaldiBrowserWindow::OnActiveTabChanged(
    content::WebContents* old_contents,
    content::WebContents* new_contents,
    int index,
    int reason) {
  UpdateTitleBar();

  infobar_container_->ChangeInfoBarManager(
      InfoBarService::FromWebContents(new_contents));

#if !defined(OS_MACOSX)

  // TODO(pettern): MOVE THIS CODE TO ExtensionActionUtil::ActiveTabChanged,
  // this does not belong here.
  //
  // Make sure we do a re-layout to keep all webviews in size-sync. Scenario; if
  // the user switch tab in fullscreen mode, only
  // BrowserPlugin::UpdateVisibility is called. We need to make sure
  // BrowserPlugin::UpdateGeometry is called. Note this was not visible prior to
  // the change we have in BrowserPlugin::UpdateGeometry where a lot of extra
  // resizing on hidden WebViews was done, and we want to be lazy. See VB-29032.
  content::WebContentsImpl* web_contents =
      static_cast<content::WebContentsImpl*>(new_contents);
  content::BrowserPluginGuest* guestplugin =
      web_contents->GetBrowserPluginGuest();
  // Only do this in fullscreen as switching tabs in windowed mode will cause a
  // relayout through RenderWidgetCompositor::UpdateLayerTreeHost() (This is due
  // to painting optimization.)
  bool is_fullscreen_without_chrome = IsFullscreen();
  if (guestplugin && is_fullscreen_without_chrome) {
    gfx::Rect frame_rect = guestplugin->frame_rect();
    gfx::Rect bounds = GetBounds();
    gfx::Size window_size = gfx::Size(bounds.width(), bounds.height());
    if (window_size != gfx::Size(frame_rect.x() + frame_rect.width(),
                                 frame_rect.y() + frame_rect.height())) {
      guestplugin->SizeContents(window_size);
    }
  }
#endif  // !OS_MACOSX
}

void VivaldiBrowserWindow::SetWebContentsBlocked(
    content::WebContents* web_contents,
    bool blocked) {
  app_delegate_->SetWebContentsBlocked(web_contents, blocked);
}

bool VivaldiBrowserWindow::IsWebContentsVisible(
    content::WebContents* web_contents) {
  return app_delegate_->IsWebContentsVisible(web_contents);
}

web_modal::WebContentsModalDialogHost*
VivaldiBrowserWindow::GetWebContentsModalDialogHost() {
  return native_app_window_.get();
}

void VivaldiBrowserWindow::EnterFullscreen(
    const GURL& url,
    ExclusiveAccessBubbleType bubble_type) {
  SetFullscreen(true);
}

void VivaldiBrowserWindow::ExitFullscreen() {
  SetFullscreen(false);
}

void VivaldiBrowserWindow::SetFullscreen(bool enable) {
  native_app_window_->SetFullscreen(
      enable ? extensions::AppWindow::FULLSCREEN_TYPE_WINDOW_API
             : extensions::AppWindow::FULLSCREEN_TYPE_NONE);
}

bool VivaldiBrowserWindow::IsFullscreen() const {
  return native_app_window_.get() ? native_app_window_->IsFullscreen() : false;
}

extensions::WindowController*
VivaldiBrowserWindow::GetExtensionWindowController() const {
  return app_window_contents_->GetWindowController();
}

content::WebContents* VivaldiBrowserWindow::GetAssociatedWebContents() const {
  return web_contents();
}

// infobars::InfoBarContainer::Delegate, none of these methods are used by us.
void VivaldiBrowserWindow::InfoBarContainerStateChanged(bool is_animating) {}

content::WebContents* VivaldiBrowserWindow::GetActiveWebContents() const {
  return browser_->tab_strip_model()->GetActiveWebContents();
}

// VivaldiWindowEventDelegate implementation
void VivaldiBrowserWindow::OnMinimizedChanged(bool minimized) {
  if (browser_.get() == nullptr) {
    return;
  }
  ::vivaldi::BroadcastEvent(
      extensions::vivaldi::window_private::OnMinimized::kEventName,
      extensions::vivaldi::window_private::OnMinimized::Create(
          browser_->session_id().id(), minimized),
      browser_->profile());
}

void VivaldiBrowserWindow::OnMaximizedChanged(bool maximized) {
  if (browser_.get() == nullptr) {
    return;
  }
  ::vivaldi::BroadcastEvent(
      extensions::vivaldi::window_private::OnMaximized::kEventName,
      extensions::vivaldi::window_private::OnMaximized::Create(
          browser_->session_id().id(), maximized),
      browser_->profile());
}

void VivaldiBrowserWindow::OnFullscreenChanged(bool fullscreen) {
  ::vivaldi::BroadcastEvent(
      extensions::vivaldi::window_private::OnFullscreen::kEventName,
      extensions::vivaldi::window_private::OnFullscreen::Create(
          browser_->session_id().id(), fullscreen),
      browser_->profile());
}

void VivaldiBrowserWindow::OnActivationChanged(bool activated) {
  ::vivaldi::BroadcastEvent(
      extensions::vivaldi::window_private::OnActivated::kEventName,
      extensions::vivaldi::window_private::OnActivated::Create(
          browser_->session_id().id(), activated),
      browser_->profile());
}

void VivaldiBrowserWindow::OnPositionChanged() {
  ::vivaldi::BroadcastEvent(
      extensions::vivaldi::window_private::OnPositionChanged::kEventName,
      extensions::vivaldi::window_private::OnPositionChanged::Create(
          browser_->session_id().id()),
      browser_->profile());
}

void VivaldiBrowserWindow::OnDocumentLoaded() {
  has_loaded_document_ = true;
  Show();
}

PageActionIconContainer* VivaldiBrowserWindow::GetOmniboxPageActionIconContainer() {
  return page_action_icon_container_.get();
}

PageActionIconContainer* VivaldiBrowserWindow::GetToolbarPageActionIconContainer() {
  if (!page_action_icon_container_) {
    page_action_icon_container_.reset(new VivaldiPageActionIconContainer(
        browser_->profile(), browser_.get()));
  }
  return page_action_icon_container_.get();
}

autofill::LocalCardMigrationBubble*
VivaldiBrowserWindow::ShowLocalCardMigrationBubble(
    content::WebContents* contents,
    autofill::LocalCardMigrationBubbleController* controller,
    bool is_user_gesture) {
  return nullptr;
}

bool VivaldiBrowserWindow::DoBrowserControlsShrinkRendererSize(
  const content::WebContents* contents) const {
  return false;
}

int VivaldiBrowserWindow::GetTopControlsHeight() const {
  return 0;
}

bool VivaldiBrowserWindow::CanUserExitFullscreen() const {
  return true;
}

VivaldiBrowserWindow::VivaldiManagePasswordsIconView::
    VivaldiManagePasswordsIconView(Profile* profile, Browser* browser)
    : profile_(profile), browser_(browser) {}

void VivaldiBrowserWindow::VivaldiManagePasswordsIconView::SetState(
  password_manager::ui::State state) {
  DCHECK(profile_);
  extensions::VivaldiUtilitiesAPI* utils_api =
    extensions::VivaldiUtilitiesAPI::GetFactoryInstance()->Get(
      profile_);
  bool show = state == password_manager::ui::State::PENDING_PASSWORD_STATE;
  show = state != password_manager::ui::State::INACTIVE_STATE;
  utils_api->OnPasswordIconStatusChanged(browser_->session_id().id(), show);
}

void VivaldiBrowserWindow::VivaldiManagePasswordsIconView::Update() {
  // contents can be null when we recover after UI process crash.
  content::WebContents* web_contents =
      browser_->tab_strip_model()->GetActiveWebContents();
  if (web_contents) {
    ManagePasswordsUIController::FromWebContents(web_contents)
        ->UpdateIconAndBubbleState(this);
  }
}

VivaldiBrowserWindow::VivaldiPageActionIconContainer::
    VivaldiPageActionIconContainer(Profile* profile, Browser* browser)
    : profile_(profile), browser_(browser) {}

VivaldiBrowserWindow::VivaldiPageActionIconContainer::
    ~VivaldiPageActionIconContainer() {}

void VivaldiBrowserWindow::VivaldiPageActionIconContainer::UpdatePageActionIcon(
    PageActionIconType type) {

  if (type == PageActionIconType::kManagePasswords) {
    if (!icon_view_) {
      icon_view_.reset(new VivaldiManagePasswordsIconView(profile_, browser_));
    }
    icon_view_->Update();
  }
}

void VivaldiBrowserWindow::VivaldiPageActionIconContainer::
    ExecutePageActionIconForTesting(PageActionIconType type) {}

send_tab_to_self::SendTabToSelfBubbleView*
VivaldiBrowserWindow::ShowSendTabToSelfBubble(
    content::WebContents* contents,
    send_tab_to_self::SendTabToSelfBubbleController* controller,
    bool is_user_gesture) {
  return nullptr;
}

void VivaldiBrowserWindow::NavigationStateChanged(
    content::WebContents* source,
    content::InvalidateTypes changed_flags) {
  if (changed_flags & content::INVALIDATE_TYPE_LOAD) {
    if (source == GetActiveWebContents()) {
      int windowId = browser()->session_id().id();
      base::string16 statustext =
          CoreTabHelper::FromWebContents(source)->GetStatusText();
      ::vivaldi::BroadcastEvent(
          extensions::vivaldi::window_private::OnActiveTabStatusText::
              kEventName,
          extensions::vivaldi::window_private::OnActiveTabStatusText::Create(
              windowId, base::UTF16ToUTF8(statustext)),
          GetProfile());
    }
  }
}

ExtensionsContainer* VivaldiBrowserWindow::GetExtensionsContainer() {
  return nullptr;
}

ui::ZOrderLevel VivaldiBrowserWindow::GetZOrderLevel() const {
  return ui::ZOrderLevel::kNormal;
}
