// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SEARCH_INSTANT_SERVICE_H_
#define CHROME_BROWSER_SEARCH_INSTANT_SERVICE_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/optional.h"
#include "build/build_config.h"
#include "chrome/browser/search/background/ntp_background_service_observer.h"
#include "chrome/browser/search/search_provider_observer.h"
#include "components/history/core/browser/history_types.h"
#include "components/image_fetcher/core/image_fetcher_impl.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/ntp_tiles/most_visited_sites.h"
#include "components/ntp_tiles/ntp_tile.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_registry_simple.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"
#include "url/gurl.h"

#if defined(OS_ANDROID)
#error "Instant is only used on desktop";
#endif

class InstantIOContext;
class InstantServiceObserver;
class NtpBackgroundService;
class Profile;
struct CollectionImage;
struct InstantMostVisitedInfo;
struct ThemeBackgroundInfo;

namespace content {
class RenderProcessHost;
}  // namespace content

extern const char kNtpCustomBackgroundMainColor[];

// Tracks render process host IDs that are associated with Instant, i.e.
// processes that are used to render an NTP. Also responsible for keeping
// necessary information (most visited tiles and theme info) updated in those
// renderer processes.
class InstantService : public KeyedService,
                       public NtpBackgroundServiceObserver,
                       public content::NotificationObserver,
                       public ntp_tiles::MostVisitedSites::Observer,
                       public ui::NativeThemeObserver {
 public:
  explicit InstantService(Profile* profile);
  ~InstantService() override;

  // Add, remove, and query RenderProcessHost IDs that are associated with
  // Instant processes.
  void AddInstantProcess(int process_id);
  bool IsInstantProcess(int process_id) const;

  // Adds/Removes InstantService observers.
  void AddObserver(InstantServiceObserver* observer);
  void RemoveObserver(InstantServiceObserver* observer);

  // Register prefs associated with the NTP.
  static void RegisterProfilePrefs(PrefRegistrySimple* registry);

#if defined(UNIT_TEST)
  int GetInstantProcessCount() const {
    return process_ids_.size();
  }
#endif

  // Invoked whenever an NTP is opened. Causes an async refresh of Most Visited
  // items.
  void OnNewTabPageOpened();

  // Most visited item APIs.
  //
  // Invoked when the Instant page wants to delete a Most Visited item.
  void DeleteMostVisitedItem(const GURL& url);
  // Invoked when the Instant page wants to undo the deletion.
  void UndoMostVisitedDeletion(const GURL& url);
  // Invoked when the Instant page wants to undo all Most Visited deletions.
  void UndoAllMostVisitedDeletions();
  // Invoked when the Instant page wants to add a custom link.
  bool AddCustomLink(const GURL& url, const std::string& title);
  // Invoked when the Instant page wants to update a custom link.
  bool UpdateCustomLink(const GURL& url,
                        const GURL& new_url,
                        const std::string& new_title);
  // Invoked when the Instant page wants to reorder a custom link.
  bool ReorderCustomLink(const GURL& url, int new_pos);
  // Invoked when the Instant page wants to delete a custom link.
  bool DeleteCustomLink(const GURL& url);
  // Invoked when the Instant page wants to undo the previous custom link
  // action. Returns false and does nothing if the profile is using a third-
  // party NTP.
  bool UndoCustomLinkAction();
  // Invoked when the Instant page wants to delete all custom links and use Most
  // Visited sites instead. Returns false and does nothing if the profile is
  // using a third-party NTP. Marked virtual for mocking in tests.
  virtual bool ResetCustomLinks();
  // Invoked when the Instant page wants to switch between custom links and Most
  // Visited. Toggles between the two options each time it's called. Returns
  // false and does nothing if the profile is using a third-party NTP.
  bool ToggleMostVisitedOrCustomLinks();
  // Invoked when the Instant page wants to toggle visibility of the tiles.
  // Notifies observers only if |do_notify| is true, which is usually the case
  // if |ToggleMostVisitedOrCustomLinks| will not be called immediately after.
  // Returns false and does nothing if the profile is using a third-party NTP.
  bool ToggleShortcutsVisibility(bool do_notify);

  // Invoked to update theme information for the NTP.
  void UpdateThemeInfo();

  // Invoked when a background pref update is received via sync, triggering
  // an update of theme info.
  void UpdateBackgroundFromSync();

  // Invoked by the InstantController to update most visited items details for
  // NTP.
  void UpdateMostVisitedInfo();

  // Sends the current NTP URL to a renderer process.
  void SendNewTabPageURLToRenderer(content::RenderProcessHost* rph);

  // Invoked when a custom background is selected on the NTP.
  void SetCustomBackgroundURL(const GURL& url);

  // Invoked when a custom background is configured on the NTP.
  void SetCustomBackgroundInfo(const GURL& background_url,
                               const std::string& attribution_line_1,
                               const std::string& attribution_line_2,
                               const GURL& action_url,
                               const std::string& collection_id);

  // Invoked when a user selected the "Upload an image" option on the NTP.
  void SelectLocalBackgroundImage(const base::FilePath& path);

  // Getter for |theme_info_| that will also initialize it if necessary.
  ThemeBackgroundInfo* GetInitializedThemeInfo();

  // Used for testing.
  void SetNativeThemeForTesting(ui::NativeTheme* theme);

  // Used for testing.
  void AddValidBackdropUrlForTesting(const GURL& url) const;

  // Used for testing.
  void AddValidBackdropCollectionForTesting(
      const std::string& collection_id) const;

  // Used for testing.
  void SetNextCollectionImageForTesting(const CollectionImage& image) const;

  // Check if a custom background has been set by the user.
  bool IsCustomBackgroundSet();

  // Returns whether the user has customized their shortcuts. Will always be
  // false if Most Visited shortcuts are enabled.
  bool AreShortcutsCustomized();

  // Returns the current shortcut settings as a pair consisting of shortcut type
  // (i.e. true if Most Visited, false if custom links) and visibility. These
  // correspond to values stored in |kNtpUseMostVisitedTiles| and
  // |kNtpShortcutsVisible| respectively.
  std::pair<bool, bool> GetCurrentShortcutSettings();

  // Reset all NTP customizations to default. Marked virtual for mocking in
  // tests.
  virtual void ResetToDefault();

  // Calculates the most frequent color of the image and stores it in prefs.
  void UpdateCustomBackgroundColorAsync(
      base::TimeTicks timestamp,
      const gfx::Image& fetched_image,
      const image_fetcher::RequestMetadata& metadata);

  // Fetches the image for the given |fetch_url|.
  void FetchCustomBackground(base::TimeTicks timestamp, const GURL& fetch_url);

 private:
  friend class InstantExtendedTest;
  friend class InstantUnitTestBase;
  friend class LocalNTPBackgroundsAndDarkModeTest;
  friend class TestInstantService;

  FRIEND_TEST_ALL_PREFIXES(InstantExtendedTest, ProcessIsolation);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, GetNTPTileSuggestion);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest,
                           DoesToggleMostVisitedOrCustomLinks);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, DoesToggleShortcutsVisibility);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, IsCustomLinksEnabled);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, TestNoThemeInfo);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, TestUpdateCustomBackgroundColor);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest,
                           LocalImageDoesNotUpdateCustomBackgroundColor);
  FRIEND_TEST_ALL_PREFIXES(InstantServiceTest, RefreshesBackgroundAfter24Hours);

  // KeyedService:
  void Shutdown() override;

  // NtpBackgroundServiceObserver:
  void OnCollectionInfoAvailable() override {}
  void OnCollectionImagesAvailable() override {}
  void OnNextCollectionImageAvailable() override;
  void OnNtpBackgroundServiceShuttingDown() override;

  // content::NotificationObserver:
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

  // Called when a renderer process is terminated.
  void OnRendererProcessTerminated(int process_id);

  // ui::NativeThemeObserver:
  void OnNativeThemeUpdated(ui::NativeTheme* observed_theme) override;

  // Called when the search provider changes. Disables custom links if the
  // search provider is not Google.
  void OnSearchProviderChanged();

  // ntp_tiles::MostVisitedSites::Observer implementation.
  void OnURLsAvailable(
      const std::map<ntp_tiles::SectionType, ntp_tiles::NTPTilesVector>&
          sections) override;
  void OnIconMadeAvailable(const GURL& site_url) override;

  void NotifyAboutMostVisitedInfo();
  void NotifyAboutThemeInfo();

  // Returns true if this is a Google NTP and the user has chosen to show custom
  // links.
  bool IsCustomLinksEnabled();

  void BuildThemeInfo();

  void ApplyOrResetCustomBackgroundThemeInfo();

  void ApplyCustomBackgroundThemeInfo();

  // Marked virtual for mocking in tests.
  virtual void ResetCustomBackgroundThemeInfo();

  void FallbackToDefaultThemeInfo();

  void RemoveLocalBackgroundImageCopy();

  // Returns false if the custom background pref cannot be parsed, otherwise
  // returns true and sets custom_background_url to the value in the pref.
  bool IsCustomBackgroundPrefValid(GURL& custom_background_url);

  // Update the background pref to point to
  // chrome-search://local-ntp/background.jpg
  void SetBackgroundToLocalResource();

  // Updates custom background prefs with color if the background hasn't changed
  // since the calculation started.
  void UpdateCustomBackgroundPrefsWithColor(base::TimeTicks timestamp,
                                            SkColor color);

  void SetImageFetcherForTesting(image_fetcher::ImageFetcher* image_fetcher);

  void SetClockForTesting(base::Clock* clock);

  base::TimeTicks GetBackgroundUpdatedTimestampForTesting() {
    return background_updated_timestamp_;
  }

  // Requests a new background image if it hasn't been updated in >24 hours.
  void RefreshBackgroundIfNeeded();

  Profile* const profile_;

  // The process ids associated with Instant processes.
  std::set<int> process_ids_;

  // Contains InstantMostVisitedItems received from |most_visited_sites_| and
  // information required to display NTP tiles.
  std::unique_ptr<InstantMostVisitedInfo> most_visited_info_;

  // Theme-related data for NTP overlay to adopt themes.
  std::unique_ptr<ThemeBackgroundInfo> theme_info_;

  base::ObserverList<InstantServiceObserver>::Unchecked observers_;

  content::NotificationRegistrar registrar_;

  scoped_refptr<InstantIOContext> instant_io_context_;

  // Data source for NTP tiles (aka Most Visited tiles). May be null.
  std::unique_ptr<ntp_tiles::MostVisitedSites> most_visited_sites_;

  // Keeps track of any changes in search engine provider. May be null.
  std::unique_ptr<SearchProviderObserver> search_provider_observer_;

  PrefChangeRegistrar pref_change_registrar_;

  PrefService* pref_service_;

  ScopedObserver<ui::NativeTheme, InstantService> theme_observer_;

  ScopedObserver<NtpBackgroundService, NtpBackgroundServiceObserver>
      background_service_observer_;

  ui::NativeTheme* native_theme_;

  NtpBackgroundService* background_service_;

  std::unique_ptr<image_fetcher::ImageFetcher> image_fetcher_;

  base::TimeTicks background_updated_timestamp_;

  base::Clock* clock_;

  base::WeakPtrFactory<InstantService> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(InstantService);
};

#endif  // CHROME_BROWSER_SEARCH_INSTANT_SERVICE_H_
