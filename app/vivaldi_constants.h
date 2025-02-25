// Copyright (c) 2017 Vivaldi Technologies AS. All rights reserved

// Defines all the command-line switches used by Vivaldi.

#ifndef APP_VIVALDI_CONSTANTS_H_
#define APP_VIVALDI_CONSTANTS_H_

#define VIVALDI_APP_ID "mpognobbkildjkofajifpdfhcoklimli"
#define VIVALDI_APP_HEX_ID "A414F2F4543AC9C68DDF663D0A459E6053E3373C"

#define VIVALDI_DATA_URL_SCHEME "chrome"

#define VIVALDI_DATA_URL_HOST "vivaldi-data"
#define VIVALDI_DATA_URL_MAPPING_DIR "/local-image/"
#define VIVALDI_WEBUI_URL_HOST "vivaldi-webui"
#define VIVALDI_THUMB_URL_HOST "thumb"

namespace vivaldi {

// All constants in alphabetical order. The constants should be documented
// alongside the definition of their values in the .cc file.

extern const char kExtDataKey[];

extern const char kNicknameExists[];

extern const char kVivaldiAppId[];
extern const char kVivaldiAppIdHex[];

extern const char kVivaldiAppURLDomain[];
extern const char kVivaldiNewTabURL[];
extern const char kVivaldiIncognitoURL[];
extern const char kVivaldiGuestSessionURL[];

extern const char kVivaldiReservedApiError[];

extern const char kVivaldiUIScheme[];

extern const char kVivaldiUIDataHost[];
extern const char kVivaldiWebUIHost[];
extern const char kVivaldiThumbDataHost[];
extern const char kVivaldiUIDataURL[];
extern const char kBaseFileMappingUrl[];
extern const char kVivaldiWebUIURL[];
extern const char kVivaldiThumbURL[];

extern const char kWindowExtDataKey[];

extern const wchar_t kUpdateNotifierAutorunName[];

}  // namespace vivaldi

#endif  // APP_VIVALDI_CONSTANTS_H_
