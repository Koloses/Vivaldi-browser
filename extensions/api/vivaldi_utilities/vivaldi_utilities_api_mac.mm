//
// Copyright (c) 2015 Vivaldi Technologies AS. All rights reserved.
//

#include "extensions/api/vivaldi_utilities/vivaldi_utilities_api.h"

#include <Cocoa/Cocoa.h>

namespace {
NSString* vivaldi_uuid_key = @"vivaldi_user_id";
}  // anonymous namespace

namespace extensions {
bool UtilitiesGetUniqueUserIdFunction::ReadUserIdFromOSProfile(
    std::string* user_id) {
  NSUserDefaults* userDefaults = [NSUserDefaults standardUserDefaults];
  NSString* value = [userDefaults stringForKey:vivaldi_uuid_key];

  if (value == nil)
    return false;

  user_id->assign([value UTF8String]);
  return true;
}

void UtilitiesGetUniqueUserIdFunction::WriteUserIdToOSProfile(
    const std::string& user_id) {
  NSUserDefaults* userDefaults = [NSUserDefaults standardUserDefaults];
  NSString* value = [NSString stringWithUTF8String:user_id.c_str()];
  [userDefaults setObject:value forKey:vivaldi_uuid_key];
  [userDefaults synchronize];
}

bool UtilitiesGetSystemDateFormatFunction::ReadDateFormats(
    vivaldi::utilities::DateFormats* date_formats) {
  NSLocale* locale = [NSLocale autoupdatingCurrentLocale];
  NSCalendar* calendar = [locale objectForKey:NSLocaleCalendar];
  // macOS weekdays start on 1, moment.js starts on 0.
  date_formats->first_day_of_week = [calendar firstWeekday] - 1;

  NSUserDefaults* userDefaults = [NSUserDefaults standardUserDefaults];
  NSDictionary* dateFormatstrings =
        [userDefaults dictionaryForKey:@"AppleICUDateFormatStrings"];

  if (dateFormatstrings[@"1"]) {
    NSString* value = dateFormatstrings[@"1"];
    date_formats->short_date_format = [value UTF8String];
  } else {
    NSString* shortDateTemplate = @"d.M.yyyy";
    NSString* shortDateFormat = [NSDateFormatter
        dateFormatFromTemplate:shortDateTemplate options:0 locale:locale];
    date_formats->short_date_format = [shortDateFormat UTF8String];
  }

  if (dateFormatstrings[@"4"]) {
    NSString* value = dateFormatstrings[@"4"];
    date_formats->long_date_format = [value UTF8String];
  } else {
    NSString* longDateTemplate = @"EEEE, d. MMMM yyyy";
    NSString* longDateFormat = [NSDateFormatter
        dateFormatFromTemplate:longDateTemplate options:0 locale:locale];
    date_formats->long_date_format = [longDateFormat UTF8String];
  }

  NSString* timeTemplate =  @"HH:mm:ss";
  NSString* timeFormat = [NSDateFormatter
      dateFormatFromTemplate:timeTemplate options:0 locale:locale];
  date_formats->time_format = [timeFormat UTF8String];

  return true;
}
}  // namespace extensions
