// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/ntp/recent_tabs_page_prefs.h"

#include <jni.h>

#include "base/android/jni_string.h"
#include "chrome/android/chrome_jni_headers/RecentTabsPagePrefs_jni.h"
#include "chrome/browser/profiles/profile_android.h"
#include "chrome/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"

using base::android::ConvertJavaStringToUTF8;
using base::android::JavaParamRef;

static jlong JNI_RecentTabsPagePrefs_Init(
    JNIEnv* env,
    const JavaParamRef<jobject>& profile) {
  RecentTabsPagePrefs* recent_tabs_page_prefs =
      new RecentTabsPagePrefs(ProfileAndroid::FromProfileAndroid(profile));
  return reinterpret_cast<intptr_t>(recent_tabs_page_prefs);
}

RecentTabsPagePrefs::RecentTabsPagePrefs(Profile* profile)
    : profile_(profile) {}

void RecentTabsPagePrefs::Destroy(JNIEnv* env,
                                  const JavaParamRef<jobject>& obj) {
  delete this;
}

RecentTabsPagePrefs::~RecentTabsPagePrefs() {}

jboolean RecentTabsPagePrefs::GetSnapshotDocumentCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj) {
  return profile_->GetPrefs()->GetBoolean(prefs::kNtpCollapsedSnapshotDocument);
}

void RecentTabsPagePrefs::SetSnapshotDocumentCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    jboolean is_collapsed) {
  PrefService* prefs = profile_->GetPrefs();
  prefs->SetBoolean(prefs::kNtpCollapsedSnapshotDocument, is_collapsed);
}

jboolean RecentTabsPagePrefs::GetRecentlyClosedTabsCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj) {
  return profile_->GetPrefs()->GetBoolean(
      prefs::kNtpCollapsedRecentlyClosedTabs);
}

void RecentTabsPagePrefs::SetRecentlyClosedTabsCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    jboolean is_collapsed) {
  PrefService* prefs = profile_->GetPrefs();
  prefs->SetBoolean(prefs::kNtpCollapsedRecentlyClosedTabs, is_collapsed);
}

jboolean RecentTabsPagePrefs::GetSyncPromoCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj) {
  return profile_->GetPrefs()->GetBoolean(prefs::kNtpCollapsedSyncPromo);
}

void RecentTabsPagePrefs::SetSyncPromoCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    jboolean is_collapsed) {
  PrefService* prefs = profile_->GetPrefs();
  prefs->SetBoolean(prefs::kNtpCollapsedSyncPromo, is_collapsed);
}

jboolean RecentTabsPagePrefs::GetForeignSessionCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const JavaParamRef<jstring>& session_tag) {
  const base::DictionaryValue* dict =
      profile_->GetPrefs()->GetDictionary(prefs::kNtpCollapsedForeignSessions);
  return dict && dict->HasKey(ConvertJavaStringToUTF8(env, session_tag));
}

void RecentTabsPagePrefs::SetForeignSessionCollapsed(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const JavaParamRef<jstring>& session_tag,
    jboolean is_collapsed) {
  // Store session tags for collapsed sessions in a preference so that the
  // collapsed state persists.
  PrefService* prefs = profile_->GetPrefs();
  DictionaryPrefUpdate update(prefs, prefs::kNtpCollapsedForeignSessions);
  if (is_collapsed)
    update.Get()->SetBoolean(ConvertJavaStringToUTF8(env, session_tag), true);
  else
    update.Get()->Remove(ConvertJavaStringToUTF8(env, session_tag), NULL);
}

// static
void RecentTabsPagePrefs::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterBooleanPref(prefs::kNtpCollapsedSnapshotDocument, false);
  registry->RegisterBooleanPref(prefs::kNtpCollapsedRecentlyClosedTabs, false);
  registry->RegisterBooleanPref(prefs::kNtpCollapsedSyncPromo, false);
  registry->RegisterDictionaryPref(prefs::kNtpCollapsedForeignSessions);
}
