// Copyright (c) 2019 Vivaldi Technologies AS. All rights reserved

#include "components/adverse_adblocking/adverse_ad_filter_list.h"

#include "app/vivaldi_apptools.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/memory/singleton.h"
#include "base/path_service.h"
#include "base/sequenced_task_runner.h"
#include "base/strings/string_piece.h"
#include "base/task/post_task.h"
#include "base/time/time_to_iso8601.h"
#include "base/values.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/storage_partition.h"
#include "crypto/sha2.h"
#include "net/base/load_flags.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

#include "vivaldi/prefs/vivaldi_gen_prefs.h"

// Maximum size of checksum downloaded.
const int kMaxChecksumSize = 10 * 1024;

base::FilePath::StringType kAdverseAdFilePath =
    FILE_PATH_LITERAL("AdverseAdSiteList.json");

AdverseAdFilterListService::AdverseAdFilterListService(Profile* profile)
    : profile_(profile), weak_ptr_factory_(this) {
  if (!vivaldi::IsVivaldiRunning())
    return;
  if (profile_) {  // Profile will be null in components.
    task_runner_ = base::CreateSequencedTaskRunnerWithTraits(
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});

    PrefService* prefs = profile_->GetPrefs();

    notification_registrar_.Add(this, chrome::NOTIFICATION_PROFILE_ADDED,
                                content::Source<Profile>(profile_));

    pref_change_registrar_.Init(prefs);
    pref_change_registrar_.Add(
        vivaldiprefs::kPrivacyAdverseAdBlockEnabled,
        base::Bind(&AdverseAdFilterListService::SettingsUpdated,
                   weak_ptr_factory_.GetWeakPtr()));
  }
}

AdverseAdFilterListService::~AdverseAdFilterListService() {}

void AdverseAdFilterListService::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  switch (type) {
    case chrome::NOTIFICATION_PROFILE_ADDED: {
      // Profile and services are up and running
      OnProfileAndServicesInitialized();
      break;
    }
    default:
      NOTREACHED();
  }
}

void AdverseAdFilterListService::OnProfileAndServicesInitialized() {
  // PathExists() triggers IO restriction.
  base::ThreadRestrictions::ScopedAllowIO allow_io;

  blocklist_file_exists_ = base::PathExists(GetDefaultFilePath());

  url_loader_factory_ =
      content::BrowserContext::GetDefaultStoragePartition(profile_)
          ->GetURLLoaderFactoryForBrowserProcess();

  OnDoBlockListLifecycleCheck();
}

void AdverseAdFilterListService::DoChecksumBeforeDownload() {
  PrefService* prefs = profile_->GetPrefs();

  GURL url(
      prefs->GetString(vivaldiprefs::kPrivacyAdverseAdBlockBlockListSha256Url));
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = "GET";
  resource_request->load_flags = net::LOAD_DO_NOT_SEND_COOKIES |
                                 net::LOAD_DO_NOT_SAVE_COOKIES |
                                 net::LOAD_BYPASS_CACHE;

  // See
  // https://chromium.googlesource.com/chromium/src/+/lkgr/docs/network_traffic_annotations.md
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("vivaldi_adverse", R"(
        semantics {
          sender: "Vivaldi Adverse Ad Blocking"
          description:
            "Download SHA256 of block-list."
          trigger: "Triggered every 24 hours."
          data: "SHA256 checksum of block-list on server."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting:
            "You can enable or disable this feature via 'Block ads on abusive"
            " sites' The feature is enabled by default."
          chrome_policy {
            }
          }
        })");

  simple_url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);

  simple_url_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&AdverseAdFilterListService::OnSHA256SumDownloadDone,
                     base::Unretained(this)),
      kMaxChecksumSize);
}

void AdverseAdFilterListService::OnSHA256SumDownloadDone(
    std::unique_ptr<std::string> response_body) {
  if (!sha256_sum_.empty() && response_body && sha256_sum_ != *response_body) {
    DownloadBlockList();
  }
}

void AdverseAdFilterListService::DownloadBlockList() {
  PrefService* prefs = profile_->GetPrefs();

  GURL url(prefs->GetString(vivaldiprefs::kPrivacyAdverseAdBlockBlockListUrl));
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = "GET";
  resource_request->load_flags = net::LOAD_DO_NOT_SEND_COOKIES |
                                 net::LOAD_DO_NOT_SAVE_COOKIES |
                                 net::LOAD_BYPASS_CACHE;

  // See
  // https://chromium.googlesource.com/chromium/src/+/lkgr/docs/network_traffic_annotations.md
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("vivaldi_adverse", R"(
        semantics {
          sender: "Vivaldi Adverse Ad Blocking"
          description:
            "Download of updated block-list."
          trigger: "Triggered every 24 hours."
          data: "List of sites managed by Google processed by Vivaldi"
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting:
            "You can enable or disable this feature via 'Block ads on abusive"
            " sites' The feature is enabled by default."
          chrome_policy {
            }
          }
        })");

  simple_url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);

  simple_url_loader_->DownloadToFile(
      url_loader_factory_.get(),
      base::BindOnce(&AdverseAdFilterListService::OnBlocklistDownloadDone,
                     base::Unretained(this)),
      GetDefaultFilePath());
}

void AdverseAdFilterListService::OnBlocklistDownloadDone(
    base::FilePath response_path) {
  if (!response_path.empty()) {
    blocklist_file_exists_ = true;
    PrefService* prefs = profile_->GetPrefs();
    const base::Time now = base::Time::Now();

    prefs->SetString(vivaldiprefs::kPrivacyAdverseAdBlockLastUpdate,
                     base::TimeToISO8601(now));

    int interval =
        prefs->GetInteger(vivaldiprefs::kPrivacyAdverseAdBlockUpdateInterval);

    base::SequencedTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AdverseAdFilterListService::OnDoBlockListLifecycleCheck,
                       weak_ptr_factory_.GetWeakPtr()),
        base::TimeDelta::FromHours(interval));

    LoadList(AdverseAdFilterListService::ListFailedCallback());

  } else {
    LOG(INFO) << "Vivaldi Adverse Ad block list download failed";
  }
  simple_url_loader_.reset();
}

void AdverseAdFilterListService::OnDoBlockListLifecycleCheck() {
  PrefService* prefs = profile_->GetPrefs();
  bool doupdate = false;
  int interval =
      prefs->GetInteger(vivaldiprefs::kPrivacyAdverseAdBlockUpdateInterval);

  // If there is no file and the feature is enabled, always try to download.
  if (!blocklist_file_exists_ &&
      prefs->GetBoolean(vivaldiprefs::kPrivacyAdverseAdBlockEnabled)) {
    DownloadBlockList();
  } else {
    // check if we should update the list from server.
    const base::Time now = base::Time::Now();
    base::Time lastupdate;

    std::string lastupdatefrompref =
        prefs->GetString(vivaldiprefs::kPrivacyAdverseAdBlockLastUpdate);

    if (base::Time::FromUTCString(lastupdatefrompref.c_str(), &lastupdate)) {
      doupdate = ((now - lastupdate).InHours() >= interval);
    } else {
      doupdate = true;
    }
  }

  if (doupdate) {
    DoChecksumBeforeDownload();
  } else {
    // Schedule a new lifecyclecheck
    base::SequencedTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AdverseAdFilterListService::OnDoBlockListLifecycleCheck,
                       weak_ptr_factory_.GetWeakPtr()),
        base::TimeDelta::FromHours(interval));
  }

  // Make sure we try to load an existing file on startup.
  if (!is_enabled_and_been_loaded_ &&
      prefs->GetBoolean(vivaldiprefs::kPrivacyAdverseAdBlockEnabled)) {
    LoadList(AdverseAdFilterListService::ListFailedCallback());
  }
}

void AdverseAdFilterListService::SettingsUpdated() {
  PrefService* prefs = profile_->GetPrefs();
  if (prefs->GetBoolean(vivaldiprefs::kPrivacyAdverseAdBlockEnabled)) {
    OnDoBlockListLifecycleCheck();
  } else {
    ClearSiteList();
  }
}

void AdverseAdFilterListService::LoadList(ListFailedCallback callback) {
  LoadList(GetDefaultFilePath(), std::move(callback));
}

void AdverseAdFilterListService::LoadList(const base::FilePath& json_filename,
                                          ListFailedCallback callback) {
  base::PostTaskWithTraits(
      FROM_HERE, {content::BrowserThread::IO},
      base::BindOnce(&AdverseAdFilterListService::LoadListOnIO,
                     base::Unretained(this), json_filename,
                     std::move(callback)));
}

/*static*/
std::string* AdverseAdFilterListService::ReadFileToString(
    const base::FilePath& json_filename,
    ListFailedCallback callback) {
  std::string* json_string = new std::string;

  if (!base::ReadFileToString(json_filename, json_string)) {
    LOG(INFO) << "Loading '" << GetDefaultFilePath().AsUTF8Unsafe()
              << "' failed";
    return nullptr;
  }

  return json_string;
}

void AdverseAdFilterListService::ComputeSHA256Sum(const void* data,
                                               size_t length) {
  sha256_sum_ =
      crypto::SHA256HashString(base::StringPiece((const char*)data, length));
}

void AdverseAdFilterListService::LoadListOnIO(
    const base::FilePath& json_filename,
    ListFailedCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  auto read_json_file =
      base::BindOnce(&AdverseAdFilterListService::ReadFileToString,
                     json_filename, std::move(callback));

  auto load_json_data =
      base::BindOnce(&AdverseAdFilterListService::LoadAndInitializeFromString,
                     base::Unretained(this), std::move(callback));

  base::PostTaskAndReplyWithResult(task_runner_.get(), FROM_HERE,
                                   std::move(read_json_file),
                                   std::move(load_json_data));
}

void AdverseAdFilterListService::LoadAndInitializeFromString(
    ListFailedCallback callback,
    const std::string* json_string) {
  if (!json_string || json_string->length() == 0) {
    PostCallback(std::move(callback));
    return;
  }

  ComputeSHA256Sum(json_string->c_str(), json_string->length());

  base::Optional<base::Value> loaded_json_list =
      base::JSONReader::Read(*json_string);

  DCHECK(loaded_json_list);
  DCHECK(loaded_json_list->is_list());

  if (!loaded_json_list || !loaded_json_list->is_list()) {
    PostCallback(std::move(callback));
    return;
  }

  std::set<std::string> new_list;
  for (auto& entry : loaded_json_list->GetList()) {
    if (!entry.is_dict())
      continue;

    auto* hostname_entry = entry.FindKey("reviewedSite");
    if (!hostname_entry || !hostname_entry->is_string())
      continue;

    std::string hostname = hostname_entry->GetString();

    if (hostname.find_first_of(":/") != std::string::npos)
      continue;

    new_list.insert(hostname);
  }

  DCHECK(!new_list.empty());

  adverse_ad_sites_ = new_list;
}

bool AdverseAdFilterListService::IsSiteInList(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS())
    return false;

  size_t pos = 0;
  std::string hostname = url.host();
  while (pos != std::string::npos && pos < hostname.length()) {
    if (adverse_ad_sites_.find(hostname.substr(pos)) != adverse_ad_sites_.end())
      return true;

    pos = hostname.find_first_of('.', pos);
    if (pos != std::string::npos)
      pos++;
  }
  return false;
}

void AdverseAdFilterListService::AddBlockItem(const std::string& new_site) {
  DCHECK(!new_site.empty());
  DCHECK(adverse_ad_sites_.find(new_site) == adverse_ad_sites_.end());
  adverse_ad_sites_.insert(new_site);
}

void AdverseAdFilterListService::ClearSiteList() {
  adverse_ad_sites_.clear();
}

/*static*/
void AdverseAdFilterListService::PostCallback(ListFailedCallback callback) {
  if (callback.is_null())
    return;
  base::PostTaskWithTraits(FROM_HERE, {content::BrowserThread::UI},
                           std::move(callback));
}

// static
base::FilePath AdverseAdFilterListService::GetDefaultFilePath() {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir))
    return base::FilePath();
  return user_data_dir.Append(kAdverseAdFilePath);
}
