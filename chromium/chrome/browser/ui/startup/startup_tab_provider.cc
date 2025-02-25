// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/startup/startup_tab_provider.h"

#include "base/metrics/histogram_macros.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/profile_resetter/triggered_profile_resetter.h"
#include "chrome/browser/profile_resetter/triggered_profile_resetter_factory.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/browser/signin/signin_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/pinned_tab_codec.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/primary_account_mutator.h"
#include "net/base/url_util.h"

#if defined(OS_WIN)
#include "base/win/windows_version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/shell_integration.h"
#endif  // defined(OS_WIN)

#if defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)
#include "chrome/browser/ui/webui/welcome/nux_helper.h"
#endif  // defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)

#include "app/vivaldi_apptools.h"
#include "app/vivaldi_constants.h"
#include "chrome/grit/locale_settings.h"
#include "ui/base/l10n/l10n_util.h"
#include "vivaldi/prefs/vivaldi_gen_prefs.h"

namespace {

// Attempts to find an existing, non-empty tabbed browser for this profile.
bool ProfileHasOtherTabbedBrowser(Profile* profile) {
  BrowserList* browser_list = BrowserList::GetInstance();
  auto other_tabbed_browser = std::find_if(
      browser_list->begin(), browser_list->end(), [profile](Browser* browser) {
        return browser->profile() == profile && browser->is_type_tabbed() &&
               !browser->tab_strip_model()->empty();
      });
  return other_tabbed_browser != browser_list->end();
}

}  // namespace

StartupTabs StartupTabProviderImpl::GetOnboardingTabs(Profile* profile) const {
// Onboarding content has not been launched on Chrome OS.
#if defined(OS_CHROMEOS)
  return StartupTabs();
#else
  if (!profile)
    return StartupTabs();

  StandardOnboardingTabsParams standard_params;
  standard_params.is_first_run = first_run::IsChromeFirstRun();
  PrefService* prefs = profile->GetPrefs();
  standard_params.has_seen_welcome_page =
      prefs && prefs->GetBoolean(prefs::kHasSeenWelcomePage);
  standard_params.is_signin_allowed = profile->IsSyncAllowed();
  if (auto* identity_manager = IdentityManagerFactory::GetForProfile(profile)) {
    standard_params.is_signed_in = identity_manager->HasPrimaryAccount();
  }
  standard_params.is_supervised_user = profile->IsSupervised();
  standard_params.is_force_signin_enabled = signin_util::IsForceSigninEnabled();

  if (vivaldi::IsVivaldiRunning() && !standard_params.has_seen_welcome_page) {
    // Chromium sets the flag in webui code only when generating the actual
    // page. We have to do it a bit earlier.
    // NOTE(jarle@vivaldi.com): This flag is not set to true on older builds,
    // which causes VB-26089 when updating.
    prefs->SetBoolean(prefs::kHasSeenWelcomePage, true);
    // Add the welcome page here as the tabs are used in the browser creation
    // and if not set NTP is added.
    StartupTabs tabs;
    tabs.emplace_back(GetWelcomePageUrl(false), false);
    return tabs;

  }

  return GetStandardOnboardingTabsForState(standard_params);
#endif  // defined(OS_CHROMEOS)
}

StartupTabs StartupTabProviderImpl::GetWelcomeBackTabs(
    Profile* profile,
    StartupBrowserCreator* browser_creator,
    bool process_startup) const {
  StartupTabs tabs;
  if (!process_startup || !browser_creator)
    return tabs;
  if (browser_creator->welcome_back_page() &&
      CanShowWelcome(profile->IsSyncAllowed(), profile->IsSupervised(),
                     signin_util::IsForceSigninEnabled())) {
    tabs.emplace_back(GetWelcomePageUrl(false), false);
  }
  return tabs;
}

StartupTabs StartupTabProviderImpl::GetDistributionFirstRunTabs(
    StartupBrowserCreator* browser_creator) const {
  if (!browser_creator)
    return StartupTabs();
  StartupTabs tabs = GetMasterPrefsTabsForState(
      first_run::IsChromeFirstRun(), browser_creator->first_run_tabs_);
  browser_creator->first_run_tabs_.clear();
  return tabs;
}

StartupTabs StartupTabProviderImpl::GetResetTriggerTabs(
    Profile* profile) const {
  auto* triggered_profile_resetter =
      TriggeredProfileResetterFactory::GetForBrowserContext(profile);
  bool has_reset_trigger = triggered_profile_resetter &&
                           triggered_profile_resetter->HasResetTrigger();
  return GetResetTriggerTabsForState(has_reset_trigger);
}

StartupTabs StartupTabProviderImpl::GetPinnedTabs(
    const base::CommandLine& command_line,
    Profile* profile) const {
  return GetPinnedTabsForState(
      StartupBrowserCreator::GetSessionStartupPref(command_line, profile),
      PinnedTabCodec::ReadPinnedTabs(profile),
      ProfileHasOtherTabbedBrowser(profile));
}

StartupTabs StartupTabProviderImpl::GetPreferencesTabs(
    const base::CommandLine& command_line,
    Profile* profile) const {
  if (vivaldi::IsVivaldiRunning() && !ProfileHasOtherTabbedBrowser(profile)) {
    // Special handling for the vivaldi specific home page option. We need
    // access to regular preferences via the profile.
    const SessionStartupPref& pref =
        StartupBrowserCreator::GetSessionStartupPref(command_line, profile);
    if (pref.type == SessionStartupPref::VIVALDI_HOMEPAGE) {
      StartupTabs tabs;
      GURL url(
          profile->GetPrefs()->GetString(vivaldiprefs::kHomepage));
      tabs.push_back(StartupTab(url, false));
      return tabs;
    }
  }
  return GetPreferencesTabsForState(
      StartupBrowserCreator::GetSessionStartupPref(command_line, profile),
      ProfileHasOtherTabbedBrowser(profile));
}

StartupTabs StartupTabProviderImpl::GetNewTabPageTabs(
    const base::CommandLine& command_line,
    Profile* profile) const {
  return GetNewTabPageTabsForState(
      StartupBrowserCreator::GetSessionStartupPref(command_line, profile));
}

StartupTabs StartupTabProviderImpl::GetPostCrashTabs(
    bool has_incompatible_applications) const {
  return GetPostCrashTabsForState(has_incompatible_applications);
}

// static
bool StartupTabProviderImpl::CanShowWelcome(bool is_signin_allowed,
                                            bool is_supervised_user,
                                            bool is_force_signin_enabled) {
  return is_signin_allowed && !is_supervised_user && !is_force_signin_enabled;
}

// static
bool StartupTabProviderImpl::ShouldShowWelcomeForOnboarding(
    bool has_seen_welcome_page,
    bool is_signed_in) {
  return !has_seen_welcome_page && !is_signed_in;
}

// static
StartupTabs StartupTabProviderImpl::GetStandardOnboardingTabsForState(
    const StandardOnboardingTabsParams& params) {
  StartupTabs tabs;
  // NOTE(jarle@vivaldi.com): We only want to see the welcome page on first run.
  // Ref. VB-26089.
  if (vivaldi::IsVivaldiRunning() && !params.is_first_run)
    return tabs;
  if (CanShowWelcome(params.is_signin_allowed, params.is_supervised_user,
                     params.is_force_signin_enabled) &&
      ShouldShowWelcomeForOnboarding(params.has_seen_welcome_page,
                                     params.is_signed_in)) {
    tabs.emplace_back(GetWelcomePageUrl(!params.is_first_run), false);
  }
  return tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetMasterPrefsTabsForState(
    bool is_first_run,
    const std::vector<GURL>& first_run_tabs) {
  // Constants: Magic words used by Master Preferences files in place of a URL
  // host to indicate that internal pages should appear on first run.
  static constexpr char kNewTabUrlHost[] = "new_tab_page";
  static constexpr char kWelcomePageUrlHost[] = "welcome_page";

  StartupTabs tabs;
  if (is_first_run) {
    tabs.reserve(first_run_tabs.size());
    for (GURL url : first_run_tabs) {
      if (url.host_piece() == kNewTabUrlHost)
        url = GURL(chrome::kChromeUINewTabURL);
      else if (url.host_piece() == kWelcomePageUrlHost)
        url = GetWelcomePageUrl(false);
      tabs.emplace_back(url, false);
    }
  }
  return tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetResetTriggerTabsForState(
    bool profile_has_trigger) {
  StartupTabs tabs;
  if (profile_has_trigger)
    tabs.emplace_back(GetTriggeredResetSettingsUrl(), false);
  return tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetPinnedTabsForState(
    const SessionStartupPref& pref,
    const StartupTabs& pinned_tabs,
    bool profile_has_other_tabbed_browser) {
  if (pref.type == SessionStartupPref::Type::LAST ||
      profile_has_other_tabbed_browser)
    return StartupTabs();
  return pinned_tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetPreferencesTabsForState(
    const SessionStartupPref& pref,
    bool profile_has_other_tabbed_browser) {
  StartupTabs tabs;
  if (pref.type == SessionStartupPref::Type::URLS && !pref.urls.empty() &&
      !profile_has_other_tabbed_browser) {
    for (const auto& url : pref.urls)
      tabs.push_back(StartupTab(url, false));
  }
  return tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetNewTabPageTabsForState(
    const SessionStartupPref& pref) {
  if (vivaldi::IsVivaldiRunning()) {
    StartupTabs tabs;
    if (pref.type != SessionStartupPref::Type::LAST)
      tabs.emplace_back(GURL(vivaldi::kVivaldiNewTabURL), false);
    return tabs;
  }
  StartupTabs tabs;
  if (pref.type != SessionStartupPref::Type::LAST)
    tabs.emplace_back(GURL(chrome::kChromeUINewTabURL), false);
  return tabs;
}

// static
StartupTabs StartupTabProviderImpl::GetPostCrashTabsForState(
    bool has_incompatible_applications) {
  StartupTabs tabs;
#if defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)
  if (has_incompatible_applications)
    tabs.emplace_back(GetIncompatibleApplicationsUrl(), false);
#endif  // defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)
  return tabs;
}

// static
GURL StartupTabProviderImpl::GetWelcomePageUrl(bool use_later_run_variant) {
  GURL url(chrome::kChromeUIWelcomeURL);
  if (vivaldi::IsVivaldiRunning()) {
    // 'use_later_run_variant' is true with '--no-first-run' cmd line option
    if (use_later_run_variant)
      url = GURL(vivaldi::kVivaldiNewTabURL);
    else
      url = GURL(l10n_util::GetStringUTF8(IDS_WELCOME_PAGE_URL));
  }
  return use_later_run_variant
             ? net::AppendQueryParameter(url, "variant", "everywhere")
             : url;
}

#if defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)
// static
GURL StartupTabProviderImpl::GetIncompatibleApplicationsUrl() {
  UMA_HISTOGRAM_BOOLEAN("IncompatibleApplicationsPage.AddedPostCrash", true);
  GURL url(chrome::kChromeUISettingsURL);
  return url.Resolve("incompatibleApplications");
}
#endif  // defined(OS_WIN) && defined(GOOGLE_CHROME_BUILD)

// static
GURL StartupTabProviderImpl::GetTriggeredResetSettingsUrl() {
  return GURL(
      chrome::GetSettingsUrl(chrome::kTriggeredResetProfileSettingsSubPage));
}
