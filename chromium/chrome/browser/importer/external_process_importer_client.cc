// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/importer/external_process_importer_client.h"

#include <utility>

#include "base/bind.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/importer/external_process_importer_host.h"
#include "chrome/browser/importer/in_process_importer_bridge.h"
#include "chrome/common/importer/firefox_importer_utils.h"
#include "chrome/common/importer/imported_bookmark_entry.h"
#include "chrome/grit/generated_resources.h"
#include "components/strings/grit/components_strings.h"
#include "content/public/browser/system_connector.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/l10n/l10n_util.h"

#include "app/vivaldi_resources.h"
#include "importer/vivaldi_profile_import_process_messages.h"

ExternalProcessImporterClient::ExternalProcessImporterClient(
    base::WeakPtr<ExternalProcessImporterHost> importer_host,
    const importer::SourceProfile& source_profile,
    uint16_t items,
    InProcessImporterBridge* bridge)
    : total_bookmarks_count_(0),
      total_history_rows_count_(0),
      total_favicons_count_(0),
      process_importer_host_(importer_host),
      source_profile_(source_profile),
      items_(items),
      bridge_(bridge),
      cancelled_(false),
      binding_(this) {
  process_importer_host_->NotifyImportStarted();
}

void ExternalProcessImporterClient::Start() {
  AddRef();  // balanced in Cleanup.

  content::GetSystemConnector()->BindInterface(
      chrome::mojom::kProfileImportServiceName, &profile_import_);

  profile_import_.set_connection_error_handler(
      base::BindOnce(&ExternalProcessImporterClient::OnProcessCrashed, this));

  // Dictionary of all localized strings that could be needed by the importer
  // in the external process.
  base::flat_map<uint32_t, std::string> localized_strings;
  localized_strings.try_emplace(IDS_BOOKMARK_GROUP,
                                l10n_util::GetStringUTF8(IDS_BOOKMARK_GROUP));
  localized_strings.try_emplace(
      IDS_BOOKMARK_GROUP_FROM_FIREFOX,
      l10n_util::GetStringUTF8(IDS_BOOKMARK_GROUP_FROM_FIREFOX));
  localized_strings.try_emplace(
      IDS_BOOKMARK_GROUP_FROM_SAFARI,
      l10n_util::GetStringUTF8(IDS_BOOKMARK_GROUP_FROM_SAFARI));
  localized_strings.try_emplace(
      IDS_IMPORT_FROM_FIREFOX,
      l10n_util::GetStringUTF8(IDS_IMPORT_FROM_FIREFOX));
  localized_strings.try_emplace(
      IDS_IMPORT_FROM_ICEWEASEL,
      l10n_util::GetStringUTF8(IDS_IMPORT_FROM_ICEWEASEL));
  localized_strings.try_emplace(
      IDS_IMPORT_FROM_SAFARI, l10n_util::GetStringUTF8(IDS_IMPORT_FROM_SAFARI));
  localized_strings.try_emplace(
      IDS_BOOKMARK_BAR_FOLDER_NAME,
      l10n_util::GetStringUTF8(IDS_BOOKMARK_BAR_FOLDER_NAME));
  //Vivaldi
  localized_strings.try_emplace(
      IDS_BOOKMARK_GROUP_FROM_OPERA, l10n_util::GetStringUTF8(IDS_BOOKMARK_GROUP_FROM_OPERA));
  localized_strings.try_emplace(
      IDS_NOTES_GROUP_FROM_OPERA, l10n_util::GetStringUTF8(IDS_NOTES_GROUP_FROM_OPERA));
  localized_strings.try_emplace(
      IDS_IMPORTED_BOOKMARKS, l10n_util::GetStringUTF8(IDS_IMPORTED_BOOKMARKS));
  // End Vivaldi

  // If the utility process hasn't started yet the message will queue until it
  // does.
  chrome::mojom::ProfileImportObserverPtr observer;
  binding_.Bind(mojo::MakeRequest(&observer));
  profile_import_->StartImport(source_profile_, items_, localized_strings,
                               std::move(observer));
}

void ExternalProcessImporterClient::Cancel() {
  if (cancelled_)
    return;

  cancelled_ = true;
  profile_import_->CancelImport();
  CloseMojoHandles();
  Release();
}

void ExternalProcessImporterClient::OnProcessCrashed() {
  DLOG(ERROR) << __func__;
  if (cancelled_)
    return;

  // If the host is still around, cancel the import; otherwise it means the
  // import was already cancelled or completed and this message can be dropped.
  if (process_importer_host_.get())
    process_importer_host_->Cancel();
}

void ExternalProcessImporterClient::OnImportStart() {
  if (cancelled_)
    return;

  bridge_->NotifyStarted();
}

void ExternalProcessImporterClient::OnImportFinished(
    bool succeeded, const std::string& error_msg) {
  if (cancelled_)
    return;

  if (!succeeded)
    LOG(WARNING) << "Import failed.  Error: " << error_msg;
  Cleanup();
}

void ExternalProcessImporterClient::OnImportItemStart(
    importer::ImportItem import_item) {
  if (cancelled_)
    return;

  bridge_->NotifyItemStarted(import_item);
}

void ExternalProcessImporterClient::OnImportItemFinished(
    importer::ImportItem import_item) {
  if (cancelled_)
    return;

  bridge_->NotifyItemEnded(import_item);
  profile_import_->ReportImportItemFinished(import_item);
}

void ExternalProcessImporterClient::OnImportItemFailed(
    importer::ImportItem import_item,
    const std::string& error_msg) {
  if (cancelled_)
    return;

  bridge_->NotifyItemFailed(import_item, error_msg);
}

void ExternalProcessImporterClient::OnHistoryImportStart(
    uint32_t total_history_rows_count) {
  if (cancelled_)
    return;

  total_history_rows_count_ = total_history_rows_count;
  history_rows_.reserve(total_history_rows_count);
}

void ExternalProcessImporterClient::OnHistoryImportGroup(
    const std::vector<ImporterURLRow>& history_rows_group,
    int visit_source) {
  if (cancelled_)
    return;

  history_rows_.insert(history_rows_.end(), history_rows_group.begin(),
                       history_rows_group.end());
  if (history_rows_.size() >= total_history_rows_count_)
    bridge_->SetHistoryItems(history_rows_,
                             static_cast<importer::VisitSource>(visit_source));
}

void ExternalProcessImporterClient::OnHomePageImportReady(
    const GURL& home_page) {
  if (cancelled_)
    return;

  bridge_->AddHomePage(home_page);
}

void ExternalProcessImporterClient::OnBookmarksImportStart(
    const base::string16& first_folder_name,
    uint32_t total_bookmarks_count) {
  if (cancelled_)
    return;

  bookmarks_first_folder_name_ = first_folder_name;
  total_bookmarks_count_ = total_bookmarks_count;
  bookmarks_.reserve(total_bookmarks_count);
}

void ExternalProcessImporterClient::OnBookmarksImportGroup(
    const std::vector<ImportedBookmarkEntry>& bookmarks_group) {
  if (cancelled_)
    return;

  // Collect sets of bookmarks from importer process until we have reached
  // total_bookmarks_count_:
  bookmarks_.insert(bookmarks_.end(), bookmarks_group.begin(),
                    bookmarks_group.end());
  if (bookmarks_.size() >= total_bookmarks_count_)
    bridge_->AddBookmarks(bookmarks_, bookmarks_first_folder_name_);
}

void ExternalProcessImporterClient::OnNotesImportStart(
    const base::string16& first_folder_name,
    uint32_t total_notes_count) {
  if (cancelled_)
    return;

  notes_first_folder_name_ = first_folder_name;
  total_notes_count_ = total_notes_count;
  notes_.reserve(total_notes_count);
}

void ExternalProcessImporterClient::OnNotesImportGroup(
    const std::vector<ImportedNotesEntry>& notes_group) {
  if (cancelled_)
    return;

  // Collect sets of bookmarks from importer process until we have reached
  // total_bookmarks_count_:
  notes_.insert(notes_.end(), notes_group.begin(),
                    notes_group.end());
  if (notes_.size() == total_notes_count_)
    bridge_->AddNotes(notes_, notes_first_folder_name_);
}

void ExternalProcessImporterClient::OnSpeedDialImportStart(
    uint32_t total_count) {
  if (cancelled_)
    return;

  total_speeddial_count_ = total_count;
  speeddial_.reserve(total_count);
}

void ExternalProcessImporterClient::OnSpeedDialImportGroup(
    const std::vector<ImportedSpeedDialEntry>& group) {
  if (cancelled_)
    return;

  speeddial_.insert(speeddial_.end(), group.begin(), group.end());
  if (speeddial_.size() == total_speeddial_count_)
    bridge_->AddSpeedDial(speeddial_);
}

void ExternalProcessImporterClient::OnFaviconsImportStart(
    uint32_t total_favicons_count) {
  if (cancelled_)
    return;

  total_favicons_count_ = total_favicons_count;
  favicons_.reserve(total_favicons_count);
}

void ExternalProcessImporterClient::OnFaviconsImportGroup(
    const favicon_base::FaviconUsageDataList& favicons_group) {
  if (cancelled_)
    return;

  favicons_.insert(favicons_.end(), favicons_group.begin(),
                    favicons_group.end());
  if (favicons_.size() >= total_favicons_count_)
    bridge_->SetFavicons(favicons_);
}

void ExternalProcessImporterClient::OnPasswordFormImportReady(
    const autofill::PasswordForm& form) {
  if (cancelled_)
    return;

  bridge_->SetPasswordForm(form);
}

void ExternalProcessImporterClient::OnKeywordsImportReady(
    const std::vector<importer::SearchEngineInfo>& search_engines,
    bool unique_on_host_and_path) {
  if (cancelled_)
    return;
  bridge_->SetKeywords(search_engines, unique_on_host_and_path);
}

void ExternalProcessImporterClient::OnFirefoxSearchEngineDataReceived(
    const std::vector<std::string>& search_engine_data) {
  if (cancelled_)
    return;
  bridge_->SetFirefoxSearchEnginesXMLData(search_engine_data);
}

void ExternalProcessImporterClient::OnAutofillFormDataImportStart(
    uint32_t total_autofill_form_data_entry_count) {
  if (cancelled_)
    return;

  total_autofill_form_data_entry_count_ = total_autofill_form_data_entry_count;
  autofill_form_data_.reserve(total_autofill_form_data_entry_count);
}

void ExternalProcessImporterClient::OnAutofillFormDataImportGroup(
    const std::vector<ImporterAutofillFormDataEntry>&
        autofill_form_data_entry_group) {
  if (cancelled_)
    return;

  autofill_form_data_.insert(autofill_form_data_.end(),
                             autofill_form_data_entry_group.begin(),
                             autofill_form_data_entry_group.end());
  if (autofill_form_data_.size() >= total_autofill_form_data_entry_count_)
    bridge_->SetAutofillFormData(autofill_form_data_);
}

ExternalProcessImporterClient::~ExternalProcessImporterClient() {}

void ExternalProcessImporterClient::Cleanup() {
  if (cancelled_)
    return;

  if (process_importer_host_.get())
    process_importer_host_->NotifyImportEnded();
  CloseMojoHandles();
  Release();
}

void ExternalProcessImporterClient::CloseMojoHandles() {
  profile_import_.reset();
  binding_.Close();
}
