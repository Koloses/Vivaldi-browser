// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/browser/aw_browser_context.h"
#include "android_webview/browser/aw_browser_process.h"
#include "android_webview/browser/net/aw_proxy_config_monitor.h"
#include "android_webview/browser/net/aw_url_request_context_getter.h"
#include "android_webview/native_jni/AwProxyController_jni.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/feature_list.h"
#include "base/message_loop/message_loop_current.h"
#include "content/public/browser/browser_thread.h"
#include "net/proxy_resolution/proxy_config_service_android.h"
#include "services/network/public/cpp/features.h"

using base::android::AttachCurrentThread;
using base::android::HasException;
using base::android::JavaParamRef;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;
using content::BrowserThread;

namespace android_webview {

namespace {

void ProxyOverrideChanged(const JavaRef<jobject>& obj,
                          const JavaRef<jobject>& listener,
                          const JavaRef<jobject>& executor) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (listener.is_null())
    return;
  JNIEnv* env = AttachCurrentThread();
  Java_AwProxyController_proxyOverrideChanged(env, obj, listener, executor);
  if (HasException(env)) {
    // Tell the chromium message loop to not perform any tasks after the current
    // one - we want to make sure we return to Java cleanly without first making
    // any new JNI calls.
    base::MessageLoopCurrentForUI::Get()->Abort();
  }
}

}  // namespace

ScopedJavaLocalRef<jstring> JNI_AwProxyController_SetProxyOverride(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const base::android::JavaParamRef<jobjectArray>& jurl_schemes,
    const base::android::JavaParamRef<jobjectArray>& jproxy_urls,
    const base::android::JavaParamRef<jobjectArray>& jbypass_rules,
    const JavaParamRef<jobject>& listener,
    const JavaParamRef<jobject>& executor) {
  std::vector<std::string> url_schemes;
  base::android::AppendJavaStringArrayToStringVector(env, jurl_schemes,
                                                     &url_schemes);
  std::vector<std::string> proxy_urls;
  base::android::AppendJavaStringArrayToStringVector(env, jproxy_urls,
                                                     &proxy_urls);
  std::vector<net::ProxyConfigServiceAndroid::ProxyOverrideRule> proxy_rules;
  int size = url_schemes.size();
  DCHECK(url_schemes.size() == proxy_urls.size());
  proxy_rules.reserve(size);
  for (int i = 0; i < size; i++) {
    proxy_rules.emplace_back(url_schemes[i], proxy_urls[i]);
  }
  std::vector<std::string> bypass_rules;
  base::android::AppendJavaStringArrayToStringVector(env, jbypass_rules,
                                                     &bypass_rules);
  std::string result;
  if (base::FeatureList::IsEnabled(network::features::kNetworkService)) {
    result = AwProxyConfigMonitor::GetInstance()->SetProxyOverride(
        proxy_rules, bypass_rules,
        base::BindOnce(&ProxyOverrideChanged,
                       ScopedJavaGlobalRef<jobject>(env, obj),
                       ScopedJavaGlobalRef<jobject>(env, listener),
                       ScopedJavaGlobalRef<jobject>(env, executor)));
  } else {
    result =
        AwBrowserProcess::GetInstance()
            ->GetAwURLRequestContext()
            ->SetProxyOverride(
                proxy_rules, bypass_rules,
                base::BindOnce(&ProxyOverrideChanged,
                               ScopedJavaGlobalRef<jobject>(env, obj),
                               ScopedJavaGlobalRef<jobject>(env, listener),
                               ScopedJavaGlobalRef<jobject>(env, executor)));
  }
  return base::android::ConvertUTF8ToJavaString(env, result);
}

void JNI_AwProxyController_ClearProxyOverride(
    JNIEnv* env,
    const JavaParamRef<jobject>& obj,
    const JavaParamRef<jobject>& listener,
    const JavaParamRef<jobject>& executor) {
  if (base::FeatureList::IsEnabled(network::features::kNetworkService)) {
    AwProxyConfigMonitor::GetInstance()->ClearProxyOverride(base::BindOnce(
        &ProxyOverrideChanged, ScopedJavaGlobalRef<jobject>(env, obj),
        ScopedJavaGlobalRef<jobject>(env, listener),
        ScopedJavaGlobalRef<jobject>(env, executor)));
  } else {
    AwBrowserProcess::GetInstance()
        ->GetAwURLRequestContext()
        ->ClearProxyOverride(base::BindOnce(
            &ProxyOverrideChanged, ScopedJavaGlobalRef<jobject>(env, obj),
            ScopedJavaGlobalRef<jobject>(env, listener),
            ScopedJavaGlobalRef<jobject>(env, executor)));
  }
}

}  // namespace android_webview
