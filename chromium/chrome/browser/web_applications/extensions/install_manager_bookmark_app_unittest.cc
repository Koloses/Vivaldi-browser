// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/banners/app_banner_settings_helper.h"
#include "chrome/browser/extensions/convert_web_app.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_service_test_base.h"
#include "chrome/browser/extensions/launch_util.h"
#include "chrome/browser/installable/installable_metrics.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/bookmark_apps/test_web_app_provider.h"
#include "chrome/browser/web_applications/components/external_install_options.h"
#include "chrome/browser/web_applications/components/install_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "chrome/browser/web_applications/extensions/bookmark_app_install_finalizer.h"
#include "chrome/browser/web_applications/extensions/bookmark_app_registrar.h"
#include "chrome/browser/web_applications/extensions/bookmark_app_util.h"
#include "chrome/browser/web_applications/test/test_app_registrar.h"
#include "chrome/browser/web_applications/test/test_data_retriever.h"
#include "chrome/browser/web_applications/test/test_web_app_url_loader.h"
#include "chrome/browser/web_applications/web_app_install_manager.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/extensions/manifest_handlers/app_launch_info.h"
#include "chrome/common/extensions/manifest_handlers/app_theme_color_info.h"
#include "chrome/common/web_application_info.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/web_contents_tester.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_icon_set.h"
#include "extensions/common/manifest_handlers/icons_handler.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "url/gurl.h"

namespace extensions {

namespace {

const GURL kAppUrl("https://www.chromium.org/index.html");
const GURL kAppScope("https://www.chromium.org/");
const char kAppAlternativeScope[] = "http://www.chromium.org/new/";
const char kAppDefaultScope[] = "http://www.chromium.org/";
const char kAppTitle[] = "Test title";
const char kAlternativeAppTitle[] = "Different test title";
const char kAppDescription[] = "Test description";
const char kAppIconURL1[] = "http://foo.com/1.png";
const char kAppIconURL2[] = "http://foo.com/2.png";

const int kIconSizeTiny = extension_misc::EXTENSION_ICON_BITTY;
const int kIconSizeSmall = extension_misc::EXTENSION_ICON_SMALL;
const int kIconSizeMedium = extension_misc::EXTENSION_ICON_MEDIUM;
const int kIconSizeLarge = extension_misc::EXTENSION_ICON_LARGE;

SkBitmap CreateSquareBitmapWithColor(int size, SkColor color) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(size, size);
  bitmap.eraseColor(color);
  return bitmap;
}

WebApplicationInfo::IconInfo CreateIconInfoWithBitmap(int size, SkColor color) {
  WebApplicationInfo::IconInfo icon_info;
  icon_info.width = size;
  icon_info.height = size;
  icon_info.data = CreateSquareBitmapWithColor(size, color);
  return icon_info;
}

void TestAcceptDialogCallback(
    content::WebContents* initiator_web_contents,
    std::unique_ptr<WebApplicationInfo> web_app_info,
    web_app::ForInstallableSite for_installable_site,
    web_app::InstallManager::WebAppInstallationAcceptanceCallback
        acceptance_callback) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(acceptance_callback), true /*accept*/,
                                std::move(web_app_info)));
}

// Use only real BookmarkAppInstallFinalizer::FinalizeInstall and mock any other
// finalization steps as a no-operation.
class BookmarkAppInstallFinalizerInstallOnly
    : public BookmarkAppInstallFinalizer {
 public:
  using BookmarkAppInstallFinalizer::BookmarkAppInstallFinalizer;
  ~BookmarkAppInstallFinalizerInstallOnly() override = default;

  // InstallFinalizer:
  void CreateOsShortcuts(const web_app::AppId& app_id,
                         bool add_to_desktop,
                         CreateOsShortcutsCallback callback) override {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), true /*shortcuts_created*/));
  }
  void PinAppToShelf(const web_app::AppId& app_id) override {}
  void ReparentTab(const web_app::AppId& app_id,
                   content::WebContents* web_contents) override {}
  void RevealAppShim(const web_app::AppId& app_id) override {}
};

}  // namespace

class InstallManagerBookmarkAppTest : public ExtensionServiceTestBase {
 public:
  InstallManagerBookmarkAppTest() {
    scoped_feature_list_.InitWithFeatures(
        {features::kDesktopPWAsUnifiedInstall}, {});
  }

  ~InstallManagerBookmarkAppTest() override = default;

  void SetUp() override {
    ExtensionServiceTestBase::SetUp();
    InitializeEmptyExtensionService();
    service_->Init();
    EXPECT_EQ(0u, registry()->enabled_extensions().size());
    web_contents_ =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);

    auto* provider = web_app::TestWebAppProvider::Get(profile());

    auto registrar = std::make_unique<BookmarkAppRegistrar>(profile());
    registrar_ = registrar.get();

    auto install_finalizer =
        std::make_unique<BookmarkAppInstallFinalizerInstallOnly>(profile());
    install_finalizer_ = install_finalizer.get();

    auto install_manager =
        std::make_unique<web_app::WebAppInstallManager>(profile());

    install_manager->SetDataRetrieverFactoryForTesting(
        base::BindLambdaForTesting([this]() {
          // This factory requires a prepared DataRetriever. A test should
          // create one with CreateDefaultDataToRetrieve, for example.
          DCHECK(prepared_data_retriever_);
          return std::unique_ptr<web_app::WebAppDataRetriever>(
              std::move(prepared_data_retriever_));
        }));

    auto test_url_loader = std::make_unique<web_app::TestWebAppUrlLoader>();
    test_url_loader_ = test_url_loader.get();
    install_manager->SetUrlLoaderForTesting(std::move(test_url_loader));

    provider->SetRegistrar(std::move(registrar));
    provider->SetInstallManager(std::move(install_manager));
    provider->SetInstallFinalizer(std::move(install_finalizer));

    provider->Start();
  }

  void TearDown() override {
    ExtensionServiceTestBase::TearDown();
    for (content::RenderProcessHost::iterator i(
             content::RenderProcessHost::AllHostsIterator());
         !i.IsAtEnd(); i.Advance()) {
      content::RenderProcessHost* host = i.GetCurrentValue();
      if (Profile::FromBrowserContext(host->GetBrowserContext()) ==
          profile_.get())
        host->Cleanup();
    }
  }

  content::WebContents* web_contents() { return web_contents_.get(); }

  web_app::WebAppInstallManager& install_manager() {
    auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());
    return *static_cast<web_app::WebAppInstallManager*>(
        &provider->install_manager());
  }

  web_app::TestWebAppUrlLoader& url_loader() {
    DCHECK(test_url_loader_);
    return *test_url_loader_;
  }

  web_app::TestDataRetriever* data_retriever() {
    DCHECK(prepared_data_retriever_);
    return prepared_data_retriever_.get();
  }

  web_app::AppRegistrar* app_registrar() {
    DCHECK(registrar_);
    return registrar_;
  }

  void CreateEmptyDataRetriever() {
    DCHECK(!prepared_data_retriever_);
    prepared_data_retriever_ = std::make_unique<web_app::TestDataRetriever>();
  }

  void CreateDataRetrieverWithManifest(
      std::unique_ptr<blink::Manifest> manifest,
      bool is_installable) {
    CreateEmptyDataRetriever();
    data_retriever()->SetRendererWebApplicationInfo(
        std::make_unique<WebApplicationInfo>());
    data_retriever()->SetManifest(std::move(manifest), is_installable);
  }

  void CreateDataRetrieverWithRendererWebAppInfo(
      std::unique_ptr<WebApplicationInfo> web_app_info,
      bool is_installable) {
    CreateEmptyDataRetriever();

    data_retriever()->SetRendererWebApplicationInfo(std::move(web_app_info));

    auto manifest = std::make_unique<blink::Manifest>();
    data_retriever()->SetManifest(std::move(manifest), is_installable);

    web_app::IconsMap icons_map;
    icons_map[kAppUrl].push_back(
        CreateSquareBitmapWithColor(kIconSizeSmall, SK_ColorRED));
    data_retriever()->SetIcons(std::move(icons_map));
  }

  void CreateDataRetrieverWithLaunchContainer(const GURL& app_url,
                                              bool open_as_window,
                                              bool is_installable) {
    CreateEmptyDataRetriever();

    auto web_app_info = std::make_unique<WebApplicationInfo>();
    web_app_info->open_as_window = open_as_window;
    data_retriever()->SetRendererWebApplicationInfo(std::move(web_app_info));

    auto manifest = std::make_unique<blink::Manifest>();
    manifest->start_url = app_url;
    manifest->name =
        base::NullableString16(base::UTF8ToUTF16(kAppTitle), false);
    manifest->scope = GURL(kAppScope);
    data_retriever()->SetManifest(std::move(manifest), is_installable);

    data_retriever()->SetIcons(web_app::IconsMap{});
  }

  const Extension* InstallWebAppFromManifestWithFallback() {
    base::RunLoop run_loop;
    web_app::AppId app_id;

    auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());

    provider->install_manager().InstallWebAppFromManifestWithFallback(
        web_contents(),
        /*force_shortcut_app=*/false, WebappInstallSource::MENU_BROWSER_TAB,
        base::BindOnce(TestAcceptDialogCallback),
        base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                       web_app::InstallResultCode code) {
          EXPECT_EQ(web_app::InstallResultCode::kSuccess, code);
          app_id = installed_app_id;
          run_loop.Quit();
        }));

    run_loop.Run();

    const Extension* extension = service_->GetInstalledExtension(app_id);
    DCHECK(extension);
    return extension;
  }

  const Extension* InstallWebAppWithOptions(
      const web_app::ExternalInstallOptions& install_options) {
    base::RunLoop run_loop;
    web_app::AppId app_id;

    auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());

    provider->install_manager().InstallWebAppWithOptions(
        web_contents(), install_options,
        base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                       web_app::InstallResultCode code) {
          EXPECT_EQ(web_app::InstallResultCode::kSuccess, code);
          app_id = installed_app_id;
          run_loop.Quit();
        }));

    run_loop.Run();

    const Extension* extension = service_->GetInstalledExtension(app_id);
    DCHECK(extension);
    return extension;
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  std::unique_ptr<content::WebContents> web_contents_;

  BookmarkAppRegistrar* registrar_ = nullptr;
  BookmarkAppInstallFinalizerInstallOnly* install_finalizer_ = nullptr;

  web_app::TestWebAppUrlLoader* test_url_loader_ = nullptr;
  std::unique_ptr<web_app::TestDataRetriever> prepared_data_retriever_;

  DISALLOW_COPY_AND_ASSIGN(InstallManagerBookmarkAppTest);
};

TEST_F(InstallManagerBookmarkAppTest, CreateBookmarkApp) {
  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  CreateDataRetrieverWithRendererWebAppInfo(std::move(web_app_info),
                                            /*is_installable=*/false);

  const Extension* extension = InstallWebAppFromManifestWithFallback();

  EXPECT_EQ(1u, registry()->enabled_extensions().size());
  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_FALSE(extension->was_installed_by_default());
  EXPECT_FALSE(Manifest::IsPolicyLocation(extension->location()));

  EXPECT_EQ(kAppTitle, extension->name());
  EXPECT_EQ(kAppDescription, extension->description());
  EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeSmall,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
  EXPECT_FALSE(
      AppBannerSettingsHelper::GetSingleBannerEvent(
          web_contents(), kAppUrl, kAppUrl.spec(),
          AppBannerSettingsHelper::APP_BANNER_EVENT_DID_ADD_TO_HOMESCREEN)
          .is_null());
}

TEST_F(InstallManagerBookmarkAppTest, CreateBookmarkAppDefaultApp) {
  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  CreateDataRetrieverWithRendererWebAppInfo(std::move(web_app_info),
                                            /*is_installable=*/false);

  web_app::ExternalInstallOptions install_options{
      kAppUrl, web_app::LaunchContainer::kDefault,
      web_app::ExternalInstallSource::kExternalDefault};

  const Extension* extension = InstallWebAppWithOptions(install_options);

  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_TRUE(extension->was_installed_by_default());
  EXPECT_EQ(Manifest::EXTERNAL_PREF_DOWNLOAD, extension->location());
  EXPECT_FALSE(Manifest::IsPolicyLocation(extension->location()));
}

TEST_F(InstallManagerBookmarkAppTest, CreateBookmarkAppPolicyInstalled) {
  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  CreateDataRetrieverWithRendererWebAppInfo(std::move(web_app_info),
                                            /*is_installable=*/false);

  web_app::ExternalInstallOptions install_options{
      kAppUrl, web_app::LaunchContainer::kDefault,
      web_app::ExternalInstallSource::kExternalPolicy};

  const Extension* extension = InstallWebAppWithOptions(install_options);

  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_FALSE(extension->was_installed_by_default());
  EXPECT_TRUE(Manifest::IsPolicyLocation(extension->location()));
}

class InstallManagerBookmarkAppInstallableSiteTest
    : public InstallManagerBookmarkAppTest,
      public ::testing::WithParamInterface<web_app::ForInstallableSite> {
 public:
  InstallManagerBookmarkAppInstallableSiteTest() {}
  ~InstallManagerBookmarkAppInstallableSiteTest() override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InstallManagerBookmarkAppInstallableSiteTest);
};

TEST_P(InstallManagerBookmarkAppInstallableSiteTest,
       CreateBookmarkAppWithManifest) {
  auto manifest = std::make_unique<blink::Manifest>();
  manifest->start_url = kAppUrl;
  manifest->name = base::NullableString16(base::UTF8ToUTF16(kAppTitle), false);
  manifest->scope = GURL(kAppScope);
  manifest->theme_color = SK_ColorBLUE;

  const bool is_installable = GetParam() == web_app::ForInstallableSite::kYes;
  CreateDataRetrieverWithManifest(std::move(manifest), is_installable);

  const Extension* extension = InstallWebAppFromManifestWithFallback();

  EXPECT_EQ(1u, registry()->enabled_extensions().size());
  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_EQ(kAppTitle, extension->name());
  EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
  EXPECT_EQ(SK_ColorBLUE, AppThemeColorInfo::GetThemeColor(extension).value());
  EXPECT_FALSE(
      AppBannerSettingsHelper::GetSingleBannerEvent(
          web_contents(), kAppUrl, kAppUrl.spec(),
          AppBannerSettingsHelper::APP_BANNER_EVENT_DID_ADD_TO_HOMESCREEN)
          .is_null());

  if (GetParam() == web_app::ForInstallableSite::kYes) {
    EXPECT_EQ(GURL(kAppScope), GetScopeURLFromBookmarkApp(extension));
  } else {
    EXPECT_EQ(GURL(), GetScopeURLFromBookmarkApp(extension));
  }
}

TEST_P(InstallManagerBookmarkAppInstallableSiteTest,
       CreateBookmarkAppWithManifestIcons) {
  auto manifest = std::make_unique<blink::Manifest>();
  manifest->start_url = kAppUrl;
  manifest->name = base::NullableString16(base::UTF8ToUTF16(kAppTitle), false);
  manifest->scope = GURL(kAppScope);

  blink::Manifest::ImageResource icon;
  icon.src = GURL(kAppIconURL1);
  icon.purpose = {blink::Manifest::ImageResource::Purpose::ANY};
  manifest->icons.push_back(icon);
  icon.src = GURL(kAppIconURL2);
  manifest->icons.push_back(icon);

  const bool is_installable = GetParam() == web_app::ForInstallableSite::kYes;

  CreateDataRetrieverWithManifest(std::move(manifest), is_installable);

  // In the legacy system Favicon URLs were ignored by WebAppIconDownloader
  // because the site had a manifest with icons: Only 1 icon had to be
  // downloaded since the other was provided by the InstallableManager. In the
  // new extension-independent system we prefer to redownload all the icons: 2
  // icons have to be downloaded.
  data_retriever()->SetGetIconsDelegate(base::BindLambdaForTesting(
      [&](content::WebContents* web_contents,
          const std::vector<GURL>& icon_urls, bool skip_page_favicons) {
        // Instructs the downloader to not query the page for favicons.
        EXPECT_TRUE(skip_page_favicons);

        const std::vector<GURL> expected_icon_urls{GURL(kAppIconURL1),
                                                   GURL(kAppIconURL2)};
        EXPECT_EQ(expected_icon_urls, icon_urls);

        web_app::IconsMap icons_map;
        icons_map[GURL(kAppIconURL1)].push_back(
            CreateSquareBitmapWithColor(kIconSizeTiny, SK_ColorBLUE));
        icons_map[GURL(kAppIconURL2)].push_back(
            CreateSquareBitmapWithColor(kIconSizeSmall, SK_ColorRED));
        return icons_map;
      }));

  const Extension* extension = InstallWebAppFromManifestWithFallback();

  EXPECT_EQ(1u, registry()->enabled_extensions().size());
  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_EQ(kAppTitle, extension->name());
  EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));

  if (GetParam() == web_app::ForInstallableSite::kYes) {
    EXPECT_EQ(GURL(kAppScope), GetScopeURLFromBookmarkApp(extension));
  } else {
    EXPECT_EQ(GURL(), GetScopeURLFromBookmarkApp(extension));
  }
}

TEST_P(InstallManagerBookmarkAppInstallableSiteTest,
       CreateBookmarkAppWithManifestNoScope) {
  auto manifest = std::make_unique<blink::Manifest>();
  manifest->start_url = kAppUrl;
  manifest->scope = GURL(kAppDefaultScope);
  manifest->name = base::NullableString16(base::UTF8ToUTF16(kAppTitle), false);

  const bool is_installable = GetParam() == web_app::ForInstallableSite::kYes;
  CreateDataRetrieverWithManifest(std::move(manifest), is_installable);

  const Extension* extension = InstallWebAppFromManifestWithFallback();

  if (GetParam() == web_app::ForInstallableSite::kYes) {
    EXPECT_EQ(GURL(kAppDefaultScope), GetScopeURLFromBookmarkApp(extension));
  } else {
    EXPECT_EQ(GURL(), GetScopeURLFromBookmarkApp(extension));
  }
}

INSTANTIATE_TEST_SUITE_P(/* no prefix */,
                         InstallManagerBookmarkAppInstallableSiteTest,
                         ::testing::Values(web_app::ForInstallableSite::kNo,
                                           web_app::ForInstallableSite::kYes));

TEST_F(InstallManagerBookmarkAppTest,
       CreateBookmarkAppDefaultLauncherContainers) {
  {
    CreateDataRetrieverWithLaunchContainer(kAppUrl, /*open_as_window=*/true,
                                           /*is_installable=*/true);

    const Extension* extension = InstallWebAppFromManifestWithFallback();

    EXPECT_EQ(LaunchContainer::kLaunchContainerWindow,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));

    // Mark the app as not locally installed and check that it now opens in a
    // tab.
    SetBookmarkAppIsLocallyInstalled(profile(), extension, false);
    EXPECT_EQ(LaunchContainer::kLaunchContainerTab,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));

    // Mark the app as locally installed and check that it now opens in a
    // window.
    SetBookmarkAppIsLocallyInstalled(profile(), extension, true);
    EXPECT_EQ(LaunchContainer::kLaunchContainerWindow,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));
  }
  {
    CreateDataRetrieverWithLaunchContainer(GURL("https://www.example.org/"),
                                           /*open_as_window=*/false,
                                           /*is_installable=*/false);

    const Extension* extension = InstallWebAppFromManifestWithFallback();

    EXPECT_EQ(LaunchContainer::kLaunchContainerTab,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));
  }
}

TEST_F(InstallManagerBookmarkAppTest,
       CreateBookmarkAppForcedLauncherContainers) {
  {
    const GURL app_url("https://www.example.org/");
    CreateDataRetrieverWithLaunchContainer(app_url,
                                           /*open_as_window=*/true,
                                           /*is_installable=*/true);

    web_app::ExternalInstallOptions install_options{
        app_url, web_app::LaunchContainer::kTab,
        web_app::ExternalInstallSource::kInternalDefault};

    const Extension* extension = InstallWebAppWithOptions(install_options);

    EXPECT_EQ(LaunchContainer::kLaunchContainerTab,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));
  }
  {
    CreateDataRetrieverWithLaunchContainer(kAppUrl, /*open_as_window=*/false,
                                           /*is_installable=*/false);

    web_app::ExternalInstallOptions install_options{
        kAppUrl, web_app::LaunchContainer::kWindow,
        web_app::ExternalInstallSource::kInternalDefault};

    const Extension* extension = InstallWebAppWithOptions(install_options);

    EXPECT_EQ(LaunchContainer::kLaunchContainerWindow,
              GetLaunchContainer(ExtensionPrefs::Get(profile()), extension));
  }
}

TEST_F(InstallManagerBookmarkAppTest, CreateBookmarkAppWithoutManifest) {
  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);

  CreateEmptyDataRetriever();
  data_retriever()->SetRendererWebApplicationInfo(std::move(web_app_info));
  auto manifest = std::make_unique<blink::Manifest>();
  data_retriever()->SetManifest(std::move(manifest), /*is_installable=*/false);
  data_retriever()->SetIcons(web_app::IconsMap{});

  const Extension* extension = InstallWebAppFromManifestWithFallback();

  EXPECT_EQ(kAppTitle, extension->name());
  EXPECT_EQ(kAppDescription, extension->description());
  EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
  EXPECT_EQ(GURL(), GetScopeURLFromBookmarkApp(extension));
  EXPECT_FALSE(AppThemeColorInfo::GetThemeColor(extension));
}

TEST_F(InstallManagerBookmarkAppTest, CreateWebAppFromInfo) {
  CreateEmptyDataRetriever();

  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  web_app_info->scope = kAppScope;
  web_app_info->icons.push_back(
      CreateIconInfoWithBitmap(kIconSizeTiny, SK_ColorRED));

  base::RunLoop run_loop;
  web_app::AppId app_id;

  auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());

  provider->install_manager().InstallWebAppFromInfo(
      std::move(web_app_info), /*no_network_install=*/false,
      WebappInstallSource::ARC,
      base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                     web_app::InstallResultCode code) {
        EXPECT_EQ(web_app::InstallResultCode::kSuccess, code);
        app_id = installed_app_id;
        run_loop.Quit();
      }));

  run_loop.Run();

  const Extension* extension = service_->GetInstalledExtension(app_id);
  ASSERT_TRUE(extension);

  EXPECT_EQ(1u, registry()->enabled_extensions().size());
  EXPECT_TRUE(extension->from_bookmark());
  EXPECT_EQ(kAppTitle, extension->name());
  EXPECT_EQ(kAppDescription, extension->description());
  EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
  EXPECT_EQ(kAppScope, GetScopeURLFromBookmarkApp(extension));
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeTiny,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeSmall,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeSmall * 2,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeMedium,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
  EXPECT_FALSE(IconsInfo::GetIconResource(extension, kIconSizeMedium * 2,
                                          ExtensionIconSet::MATCH_EXACTLY)
                   .empty());
}

TEST_F(InstallManagerBookmarkAppTest, InstallOrUpdateWebAppFromSync) {
  CreateEmptyDataRetriever();

  EXPECT_EQ(0u, registry()->enabled_extensions().size());

  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  web_app_info->scope = GURL(kAppScope);
  web_app_info->icons.push_back(
      CreateIconInfoWithBitmap(kIconSizeSmall, SK_ColorRED));

  auto web_app_info2 = std::make_unique<WebApplicationInfo>(*web_app_info);
  web_app_info2->title = base::UTF8ToUTF16(kAlternativeAppTitle);
  web_app_info2->icons[0] =
      CreateIconInfoWithBitmap(kIconSizeLarge, SK_ColorRED);
  web_app_info2->scope = GURL(kAppAlternativeScope);

  auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile());
  web_app::AppId app_id;

  url_loader().SetNextLoadUrlResult(
      GURL("about:blank"), web_app::WebAppUrlLoader::Result::kUrlLoaded);

  {
    base::RunLoop run_loop;

    provider->install_manager().InstallOrUpdateWebAppFromSync(
        app_id, std::move(web_app_info),
        base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                       web_app::InstallResultCode code) {
          EXPECT_EQ(web_app::InstallResultCode::kSuccess, code);
          app_id = installed_app_id;
          run_loop.Quit();
        }));

    run_loop.Run();
  }

#if defined(OS_CHROMEOS)
  // On Chrome OS, sync always locally installs an app.
  const bool expect_locally_installed = true;
#else
  const bool expect_locally_installed = false;
#endif

  {
    EXPECT_EQ(1u, registry()->enabled_extensions().size());
    const Extension* extension =
        registry()->enabled_extensions().begin()->get();
    EXPECT_TRUE(extension->from_bookmark());
    EXPECT_EQ(kAppTitle, extension->name());
    EXPECT_EQ(kAppDescription, extension->description());
    EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
    EXPECT_EQ(GURL(kAppScope), GetScopeURLFromBookmarkApp(extension));
    EXPECT_FALSE(extensions::IconsInfo::GetIconResource(
                     extension, kIconSizeSmall, ExtensionIconSet::MATCH_EXACTLY)
                     .empty());
    EXPECT_EQ(expect_locally_installed,
              BookmarkAppIsLocallyInstalled(profile(), extension));
  }

  url_loader().SetNextLoadUrlResult(
      GURL("about:blank"), web_app::WebAppUrlLoader::Result::kUrlLoaded);

  // On non-ChromeOS platforms is_locally_installed case depends on IsInstalled.
  // On ChromeOS, it always behaves as if app is installed.
  EXPECT_TRUE(provider->registrar().IsInstalled(app_id));

  CreateEmptyDataRetriever();
  {
    base::RunLoop run_loop;

    provider->install_manager().InstallOrUpdateWebAppFromSync(
        app_id, std::move(web_app_info2),
        base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                       web_app::InstallResultCode code) {
          EXPECT_EQ(web_app::InstallResultCode::kSuccess, code);
          EXPECT_EQ(app_id, installed_app_id);
          run_loop.Quit();
        }));

    run_loop.Run();
  }

  {
    EXPECT_EQ(1u, registry()->enabled_extensions().size());
    const Extension* extension =
        registry()->enabled_extensions().begin()->get();
    EXPECT_TRUE(extension->from_bookmark());
    EXPECT_EQ(kAlternativeAppTitle, extension->name());
    EXPECT_EQ(kAppDescription, extension->description());
    EXPECT_EQ(kAppUrl, AppLaunchInfo::GetLaunchWebURL(extension));
    EXPECT_EQ(GURL(kAppAlternativeScope),
              GetScopeURLFromBookmarkApp(extension));
    EXPECT_FALSE(extensions::IconsInfo::GetIconResource(
                     extension, kIconSizeSmall, ExtensionIconSet::MATCH_EXACTLY)
                     .empty());
    EXPECT_FALSE(extensions::IconsInfo::GetIconResource(
                     extension, kIconSizeLarge, ExtensionIconSet::MATCH_EXACTLY)
                     .empty());
    EXPECT_TRUE(BookmarkAppIsLocallyInstalled(profile(), extension));
  }
}

TEST_F(InstallManagerBookmarkAppTest, GetAppDetails) {
  EXPECT_EQ(std::string(), app_registrar()->GetAppShortName("unknown"));
  EXPECT_EQ(GURL(), app_registrar()->GetAppLaunchURL("unknown"));
  const base::Optional<SkColor> theme_color = SK_ColorBLUE;  // 0xAABBCCDD;

  auto web_app_info = std::make_unique<WebApplicationInfo>();
  web_app_info->app_url = kAppUrl;
  web_app_info->title = base::UTF8ToUTF16(kAppTitle);
  web_app_info->description = base::UTF8ToUTF16(kAppDescription);
  web_app_info->theme_color = theme_color;
  CreateDataRetrieverWithRendererWebAppInfo(std::move(web_app_info),
                                            /*is_installable=*/false);

  const Extension* extension = InstallWebAppFromManifestWithFallback();
  EXPECT_EQ(kAppTitle, app_registrar()->GetAppShortName(extension->id()));
  EXPECT_EQ(kAppDescription,
            app_registrar()->GetAppDescription(extension->id()));
  EXPECT_EQ(theme_color, app_registrar()->GetAppThemeColor(extension->id()));
  EXPECT_EQ(kAppUrl, app_registrar()->GetAppLaunchURL(extension->id()));
}

}  // namespace extensions
