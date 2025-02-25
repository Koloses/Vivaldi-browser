// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/contextualsearch/contextual_search_tab_helper.h"

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/bind.h"
#include "build/build_config.h"
#include "chrome/android/chrome_jni_headers/ContextualSearchTabHelper_jni.h"
#include "chrome/browser/android/contextualsearch/unhandled_tap_web_contents_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_android.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_contents.h"

using base::android::JavaParamRef;
using base::android::ScopedJavaLocalRef;

ContextualSearchTabHelper::ContextualSearchTabHelper(JNIEnv* env,
                                                     jobject obj,
                                                     Profile* profile)
    : weak_java_ref_(env, obj),
      pref_change_registrar_(new PrefChangeRegistrar()),
      weak_factory_(this) {
  pref_change_registrar_->Init(profile->GetPrefs());
  pref_change_registrar_->Add(
      prefs::kContextualSearchEnabled,
      base::BindRepeating(
          &ContextualSearchTabHelper::OnContextualSearchPrefChanged,
          weak_factory_.GetWeakPtr()));
}

ContextualSearchTabHelper::~ContextualSearchTabHelper() {
  pref_change_registrar_->RemoveAll();
}

void ContextualSearchTabHelper::OnContextualSearchPrefChanged() {
  JNIEnv* env = base::android::AttachCurrentThread();
  ScopedJavaLocalRef<jobject> jobj = weak_java_ref_.get(env);
  Java_ContextualSearchTabHelper_onContextualSearchPrefChanged(env, jobj);
}

void ContextualSearchTabHelper::OnShowUnhandledTapUIIfNeeded(
    int x_px,
    int y_px,
    int font_size_dips,
    int text_run_length) {
  JNIEnv* env = base::android::AttachCurrentThread();
  ScopedJavaLocalRef<jobject> jobj = weak_java_ref_.get(env);
  Java_ContextualSearchTabHelper_onShowUnhandledTapUIIfNeeded(
      env, jobj, x_px, y_px, font_size_dips, text_run_length);
}

void ContextualSearchTabHelper::InstallUnhandledTapNotifierIfNeeded(
    JNIEnv* env,
    jobject obj,
    const JavaParamRef<jobject>& j_base_web_contents,
    jfloat device_scale_factor) {
  DCHECK(j_base_web_contents);
  content::WebContents* base_web_contents =
      content::WebContents::FromJavaWebContents(j_base_web_contents);
  DCHECK(base_web_contents);
  if (!unhandled_tap_web_contents_observer_ ||
      base_web_contents !=
          unhandled_tap_web_contents_observer_->web_contents()) {
    unhandled_tap_web_contents_observer_.reset(
        new contextual_search::UnhandledTapWebContentsObserver(
            base_web_contents, device_scale_factor,
            base::BindRepeating(
                &ContextualSearchTabHelper::OnShowUnhandledTapUIIfNeeded,
                weak_factory_.GetWeakPtr())));
  }
}

void ContextualSearchTabHelper::Destroy(JNIEnv* env,
                                        const JavaParamRef<jobject>& obj) {
  delete this;
}

static jlong JNI_ContextualSearchTabHelper_Init(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const JavaParamRef<jobject>& java_profile) {
  Profile* profile = ProfileAndroid::FromProfileAndroid(java_profile);
  CHECK(profile);
  ContextualSearchTabHelper* tab = new ContextualSearchTabHelper(
      env, obj, profile);
  return reinterpret_cast<intptr_t>(tab);
}
