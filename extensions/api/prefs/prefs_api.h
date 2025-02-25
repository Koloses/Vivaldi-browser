// Copyright (c) 2017 Vivaldi Technologies AS. All rights reserved

#ifndef EXTENSIONS_API_PREFS_PREFS_API_H_
#define EXTENSIONS_API_PREFS_PREFS_API_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/singleton.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/prefs/pref_change_registrar.h"
#include "extensions/browser/extension_function.h"
#include "prefs/vivaldi_browser_prefs.h"

class Profile;

namespace vivaldi {
class NativeSettingsObserver;
}

namespace extensions {

class VivaldiPrefsApiNotification;

class VivaldiPrefsApiNotificationFactory
    : public BrowserContextKeyedServiceFactory {
 public:
  static VivaldiPrefsApiNotificationFactory* GetInstance();

  static ::vivaldi::PrefProperties* GetPrefProperties(const std::string& path);

 private:
  friend struct base::DefaultSingletonTraits<
      VivaldiPrefsApiNotificationFactory>;
  friend class VivaldiPrefsApiNotification;

  VivaldiPrefsApiNotificationFactory();
  ~VivaldiPrefsApiNotificationFactory() override;

  // BrowserContextKeyedServiceFactory:
  KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
  content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;

  ::vivaldi::PrefsProperties prefs_properties_;
};

// A class receiving the callback notification when a registered
// prefs value has changed.
class VivaldiPrefsApiNotification : public KeyedService {
 public:
  static VivaldiPrefsApiNotification* FromBrowserContext(
      content::BrowserContext* browser_context);

  explicit VivaldiPrefsApiNotification(Profile* profile);
  ~VivaldiPrefsApiNotification() override;

  void RegisterPref(const std::string& path, bool local_pref);

  void OnChanged(const std::string& path);

 private:
  void RegisterLocalPref(const std::string& path);
  void RegisterProfilePref(const std::string& path);

  Profile* profile_;
  PrefChangeRegistrar prefs_registrar_;
  PrefChangeRegistrar local_prefs_registrar_;

  std::unique_ptr<::vivaldi::NativeSettingsObserver> native_settings_observer_;
  base::WeakPtrFactory<VivaldiPrefsApiNotification> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(VivaldiPrefsApiNotification);
};

class PrefsGetFunction : public UIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("prefs.get", PREFS_GET)
  PrefsGetFunction() = default;

 private:
  ~PrefsGetFunction() override = default;
  ResponseAction Run() override;

  DISALLOW_COPY_AND_ASSIGN(PrefsGetFunction);
};

class PrefsSetFunction : public UIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("prefs.set", PREFS_SET)
  PrefsSetFunction() = default;

 private:
  ~PrefsSetFunction() override = default;
  ResponseAction Run() override;

  DISALLOW_COPY_AND_ASSIGN(PrefsSetFunction);
};

class PrefsResetFunction : public UIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("prefs.reset", PREFS_RESET)
  PrefsResetFunction() = default;

 private:
  ~PrefsResetFunction() override = default;
  ResponseAction Run() override;

  DISALLOW_COPY_AND_ASSIGN(PrefsResetFunction);
};

class PrefsGetForCacheFunction : public UIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("prefs.getForCache", PREFS_GET_FOR_CACHE)
  PrefsGetForCacheFunction() = default;

 private:
  ~PrefsGetForCacheFunction() override = default;
  ResponseAction Run() override;

  DISALLOW_COPY_AND_ASSIGN(PrefsGetForCacheFunction);
};
}  // namespace extensions

#endif  // EXTENSIONS_API_PREFS_PREFS_API_H_
