// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/profiles/profile_avatar_icon_util.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/format_macros.h"
#include "base/macros.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkScalar.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/skia_util.h"
#include "url/url_canon.h"

// Helper methods for transforming and drawing avatar icons.
namespace {

const int kOldAvatarIconWidth = 38;
const int kOldAvatarIconHeight = 31;

// Determine what the scaled height of the avatar icon should be for a
// specified width, to preserve the aspect ratio.
int GetScaledAvatarHeightForWidth(int width, const gfx::ImageSkia& avatar) {
  // Multiply the width by the inverted aspect ratio (height over
  // width), and then add 0.5 to ensure the int truncation rounds nicely.
  int scaled_height = width *
      ((float) avatar.height() / (float) avatar.width()) + 0.5f;
  return scaled_height;
}

// A CanvasImageSource that draws a sized and positioned avatar with an
// optional border independently of the scale factor.
class AvatarImageSource : public gfx::CanvasImageSource {
 public:
  enum AvatarPosition {
    POSITION_CENTER,
    POSITION_BOTTOM_CENTER,
  };

  enum AvatarBorder {
    BORDER_NONE,
    BORDER_NORMAL,
    BORDER_ETCHED,
  };

  AvatarImageSource(gfx::ImageSkia avatar,
                    const gfx::Size& canvas_size,
                    int width,
                    AvatarPosition position,
                    AvatarBorder border,
                    profiles::AvatarShape shape);

  AvatarImageSource(gfx::ImageSkia avatar,
                    const gfx::Size& canvas_size,
                    int width,
                    AvatarPosition position,
                    AvatarBorder border);

  ~AvatarImageSource() override;

  // CanvasImageSource override:
  void Draw(gfx::Canvas* canvas) override;

 private:
  gfx::ImageSkia avatar_;
  const gfx::Size canvas_size_;
  const int width_;
  const int height_;
  const AvatarPosition position_;
  const AvatarBorder border_;
  const profiles::AvatarShape shape_;

  DISALLOW_COPY_AND_ASSIGN(AvatarImageSource);
};

AvatarImageSource::AvatarImageSource(gfx::ImageSkia avatar,
                                     const gfx::Size& canvas_size,
                                     int width,
                                     AvatarPosition position,
                                     AvatarBorder border,
                                     profiles::AvatarShape shape)
    : gfx::CanvasImageSource(canvas_size),
      canvas_size_(canvas_size),
      width_(width),
      height_(GetScaledAvatarHeightForWidth(width, avatar)),
      position_(position),
      border_(border),
      shape_(shape) {
  avatar_ = gfx::ImageSkiaOperations::CreateResizedImage(
      avatar, skia::ImageOperations::RESIZE_BEST,
      gfx::Size(width_, height_));
}

AvatarImageSource::AvatarImageSource(gfx::ImageSkia avatar,
                                     const gfx::Size& canvas_size,
                                     int width,
                                     AvatarPosition position,
                                     AvatarBorder border)
    : AvatarImageSource(avatar,
                        canvas_size,
                        width,
                        position,
                        border,
                        profiles::SHAPE_SQUARE) {}

AvatarImageSource::~AvatarImageSource() {
}

void AvatarImageSource::Draw(gfx::Canvas* canvas) {
  // Center the avatar horizontally.
  int x = (canvas_size_.width() - width_) / 2;
  int y;

  if (position_ == POSITION_CENTER) {
    // Draw the avatar centered on the canvas.
    y = (canvas_size_.height() - height_) / 2;
  } else {
    // Draw the avatar on the bottom center of the canvas, leaving 1px below.
    y = canvas_size_.height() - height_ - 1;
  }

#if defined(OS_ANDROID)
  // Circular shape is only available on desktop platforms.
  DCHECK(shape_ != profiles::SHAPE_CIRCLE);
#else
  if (shape_ == profiles::SHAPE_CIRCLE) {
    // Draw the avatar on the bottom center of the canvas; overrides the
    // previous position specification to avoid leaving visible gap below the
    // avatar.
    y = canvas_size_.height() - height_;

    // Calculate the circular mask that will be used to display the avatar
    // image.
    SkPath circular_mask;
    circular_mask.addCircle(SkIntToScalar(canvas_size_.width() / 2),
                            SkIntToScalar(canvas_size_.height() / 2),
                            SkIntToScalar(canvas_size_.width() / 2));
    canvas->ClipPath(circular_mask, true);
  }
#endif

  canvas->DrawImageInt(avatar_, x, y);

  // The border should be square.
  int border_size = std::max(width_, height_);
  // Reset the x and y for the square border.
  x = (canvas_size_.width() - border_size) / 2;
  y = (canvas_size_.height() - border_size) / 2;

  if (border_ == BORDER_NORMAL) {
    // Draw a gray border on the inside of the avatar.
    SkColor border_color = SkColorSetARGB(83, 0, 0, 0);

    // Offset the rectangle by a half pixel so the border is drawn within the
    // appropriate pixels no matter the scale factor. Subtract 1 from the right
    // and bottom sizes to specify the endpoints, yielding -0.5.
    SkPath path;
    path.addRect(SkFloatToScalar(x + 0.5f),  // left
                 SkFloatToScalar(y + 0.5f),  // top
                 SkFloatToScalar(x + border_size - 0.5f),   // right
                 SkFloatToScalar(y + border_size - 0.5f));  // bottom

    cc::PaintFlags flags;
    flags.setColor(border_color);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(SkIntToScalar(1));

    canvas->DrawPath(path, flags);
  } else if (border_ == BORDER_ETCHED) {
    // Give the avatar an etched look by drawing a highlight on the bottom and
    // right edges.
    SkColor shadow_color = SkColorSetARGB(83, 0, 0, 0);
    SkColor highlight_color = SkColorSetARGB(96, 255, 255, 255);

    cc::PaintFlags flags;
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(SkIntToScalar(1));

    SkPath path;

    // Left and top shadows. To support higher scale factors than 1, position
    // the orthogonal dimension of each line on the half-pixel to separate the
    // pixel. For a vertical line, this means adding 0.5 to the x-value.
    path.moveTo(SkFloatToScalar(x + 0.5f), SkIntToScalar(y + height_));

    // Draw up to the top-left. Stop with the y-value at a half-pixel.
    path.rLineTo(SkIntToScalar(0), SkFloatToScalar(-height_ + 0.5f));

    // Draw right to the top-right, stopping within the last pixel.
    path.rLineTo(SkFloatToScalar(width_ - 0.5f), SkIntToScalar(0));

    flags.setColor(shadow_color);
    canvas->DrawPath(path, flags);

    path.reset();

    // Bottom and right highlights. Note that the shadows own the shared corner
    // pixels, so reduce the sizes accordingly.
    path.moveTo(SkIntToScalar(x + 1), SkFloatToScalar(y + height_ - 0.5f));

    // Draw right to the bottom-right.
    path.rLineTo(SkFloatToScalar(width_ - 1.5f), SkIntToScalar(0));

    // Draw up to the top-right.
    path.rLineTo(SkIntToScalar(0), SkFloatToScalar(-height_ + 1.5f));

    flags.setColor(highlight_color);
    canvas->DrawPath(path, flags);
  }
}

}  // namespace

namespace profiles {

struct IconResourceInfo {
  int resource_id;
  const char* filename;
  int label_id;
};

constexpr int kAvatarIconSize = 96;
constexpr SkColor kAvatarTutorialBackgroundColor =
    SkColorSetRGB(0x42, 0x85, 0xf4);
constexpr SkColor kAvatarTutorialContentTextColor =
    SkColorSetRGB(0xc6, 0xda, 0xfc);
constexpr SkColor kAvatarBubbleAccountsBackgroundColor =
    SkColorSetRGB(0xf3, 0xf3, 0xf3);
constexpr SkColor kAvatarBubbleGaiaBackgroundColor =
    SkColorSetRGB(0xf5, 0xf5, 0xf5);
constexpr SkColor kUserManagerBackgroundColor = SkColorSetRGB(0xee, 0xee, 0xee);

#ifdef VIVALDI_BUILD
constexpr char kDefaultUrlPrefix[] = "chrome://theme/IDR_PROFILE_VIVALDI_AVATAR_";
constexpr char kOldDefaultUrlPrefix[] = "chrome://theme/IDR_PROFILE_AVATAR_";
// Number of our avatars to show in Chromiums creation dialog.
constexpr size_t kVivaldiAvatarsOnCreate = 15;
#else
constexpr char kDefaultUrlPrefix[] = "chrome://theme/IDR_PROFILE_AVATAR_";
#endif  // VIVALDI_BUILD
constexpr char kGAIAPictureFileName[] = "Google Profile Picture.png";
constexpr char kHighResAvatarFolderName[] = "Avatars";

// The size of the function-static kDefaultAvatarIconResources array below.
#if defined(OS_ANDROID)
constexpr size_t kDefaultAvatarIconsCount = 38;
#elif defined(OS_CHROMEOS)
constexpr size_t kDefaultAvatarIconsCount = 27;
#else
constexpr size_t kDefaultAvatarIconsCount = 38;
#endif

#if !defined(OS_ANDROID)
// The first 8 icons are generic.
constexpr size_t kGenericAvatarIconsCount = 8;
#else
constexpr size_t kGenericAvatarIconsCount = 0;
#endif

#if !defined(OS_ANDROID)
// The avatar used as a placeholder.
constexpr size_t kPlaceholderAvatarIndex = 26;
#else
constexpr size_t kPlaceholderAvatarIndex = 0;
#endif

gfx::Image GetSizedAvatarIcon(const gfx::Image& image,
                              bool is_rectangle,
                              int width,
                              int height,
                              AvatarShape shape) {
  if (!is_rectangle && image.Height() <= height)
    return image;

  gfx::Size size(width, height);

  // Source for a centered, sized icon. GAIA images get a border.
  std::unique_ptr<gfx::ImageSkiaSource> source(
      new AvatarImageSource(*image.ToImageSkia(), size, std::min(width, height),
                            AvatarImageSource::POSITION_CENTER,
                            AvatarImageSource::BORDER_NONE, shape));

  return gfx::Image(gfx::ImageSkia(std::move(source), size));
}

gfx::Image GetSizedAvatarIcon(const gfx::Image& image,
                              bool is_rectangle,
                              int width,
                              int height) {
  return GetSizedAvatarIcon(image, is_rectangle, width, height,
                            profiles::SHAPE_SQUARE);
}

gfx::Image GetAvatarIconForWebUI(const gfx::Image& image,
                                 bool is_rectangle) {
  return GetSizedAvatarIcon(image, is_rectangle, kAvatarIconSize,
                            kAvatarIconSize);
}

gfx::Image GetAvatarIconForTitleBar(const gfx::Image& image,
                                    bool is_gaia_image,
                                    int dst_width,
                                    int dst_height) {
  // The image requires no border or resizing.
  if (!is_gaia_image && image.Height() <= kAvatarIconSize)
    return image;

  int size = std::min(kAvatarIconSize, std::min(dst_width, dst_height));
  gfx::Size dst_size(dst_width, dst_height);

  // Source for a sized icon drawn at the bottom center of the canvas,
  // with an etched border (for GAIA images).
  std::unique_ptr<gfx::ImageSkiaSource> source(
      new AvatarImageSource(*image.ToImageSkia(), dst_size, size,
                            AvatarImageSource::POSITION_BOTTOM_CENTER,
                            is_gaia_image ? AvatarImageSource::BORDER_ETCHED
                                          : AvatarImageSource::BORDER_NONE));

  return gfx::Image(gfx::ImageSkia(std::move(source), dst_size));
}

SkBitmap GetAvatarIconAsSquare(const SkBitmap& source_bitmap,
                               int scale_factor) {
  SkBitmap square_bitmap;
  if ((source_bitmap.width() == scale_factor * kOldAvatarIconWidth) &&
      (source_bitmap.height() == scale_factor * kOldAvatarIconHeight)) {
    // If |source_bitmap| matches the old avatar icon dimensions, i.e. it's an
    // old avatar icon, shave a couple of columns so the |source_bitmap| is more
    // square. So when resized to a square aspect ratio it looks pretty.
    gfx::Rect frame(scale_factor * profiles::kAvatarIconSize,
                    scale_factor * profiles::kAvatarIconSize);
    frame.Inset(scale_factor * 2, 0, scale_factor * 2, 0);
    source_bitmap.extractSubset(&square_bitmap, gfx::RectToSkIRect(frame));
  } else {
    // If it's not an old avatar icon, the image should be square.
    DCHECK(source_bitmap.width() == source_bitmap.height());
    square_bitmap = source_bitmap;
  }
  return square_bitmap;
}

// Helper methods for accessing, transforming and drawing avatar icons.
size_t GetDefaultAvatarIconCount() {
  return kDefaultAvatarIconsCount;
}

size_t GetGenericAvatarIconCount() {
  return kGenericAvatarIconsCount;
}

size_t GetPlaceholderAvatarIndex() {
  return kPlaceholderAvatarIndex;
}

size_t GetModernAvatarIconStartIndex() {
#if !defined(OS_CHROMEOS) && !defined(OS_ANDROID)
  return GetPlaceholderAvatarIndex() + 1;
#else
  // Only use the placeholder avatar on ChromeOS and Android.
  // TODO(crbug.com/937834): Clean up code and remove code dependencies from
  // Android and ChromeOS. Avatar icons from this file are not used on these
  // platforms.
  return GetPlaceholderAvatarIndex();
#endif
}

bool IsModernAvatarIconIndex(size_t icon_index) {
  return icon_index >= GetModernAvatarIconStartIndex() &&
         icon_index < GetDefaultAvatarIconCount();
}

int GetPlaceholderAvatarIconResourceID() {
  return IDR_PROFILE_AVATAR_PLACEHOLDER_LARGE;
}

std::string GetPlaceholderAvatarIconUrl() {
  return "chrome://theme/IDR_PROFILE_AVATAR_PLACEHOLDER_LARGE";
}

const IconResourceInfo* GetDefaultAvatarIconResourceInfo(size_t index) {
  CHECK_LT(index, kDefaultAvatarIconsCount);
#ifdef VIVALDI_BUILD
#include "app/vivaldi_resources.h"

  // If more icons are added, we need to add them too.
  CHECK_EQ((size_t)38, kDefaultAvatarIconsCount);
  static const IconResourceInfo resource_info[kDefaultAvatarIconsCount] = {
      {IDR_PROFILE_VIVALDI_AVATAR_0, "landscape_1.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_1},
      {IDR_PROFILE_VIVALDI_AVATAR_1, "landscape_2.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_2},
      {IDR_PROFILE_VIVALDI_AVATAR_2, "landscape_3.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_3},
      {IDR_PROFILE_VIVALDI_AVATAR_3, "landscape_4.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_4},
      {IDR_PROFILE_VIVALDI_AVATAR_4, "landscape_5.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_5},
      {IDR_PROFILE_VIVALDI_AVATAR_5, "landscape_6.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_6},
      {IDR_PROFILE_VIVALDI_AVATAR_6, "landscape_7.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_7},
      {IDR_PROFILE_VIVALDI_AVATAR_7, "landscape_8.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_8},
      {IDR_PROFILE_VIVALDI_AVATAR_8, "landscape_9.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_9},
      {IDR_PROFILE_VIVALDI_AVATAR_9, "landscape_10.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_10},
      {IDR_PROFILE_VIVALDI_AVATAR_10, "landscape_11.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_11},
      {IDR_PROFILE_VIVALDI_AVATAR_11, "landscape_12.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_12},
      {IDR_PROFILE_VIVALDI_AVATAR_12, "landscape_13.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_13},
      {IDR_PROFILE_VIVALDI_AVATAR_13, "landscape_14.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_14},
      {IDR_PROFILE_VIVALDI_AVATAR_14, "landscape_15.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_15},
      {IDR_PROFILE_VIVALDI_AVATAR_15, "landscape_16.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_16},
      {IDR_PROFILE_VIVALDI_AVATAR_16, "landscape_17.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_17},
      {IDR_PROFILE_VIVALDI_AVATAR_17, "landscape_18.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_18},
      {IDR_PROFILE_VIVALDI_AVATAR_18, "monster_1.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_19},
      {IDR_PROFILE_VIVALDI_AVATAR_19, "monster_2.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_20},
      {IDR_PROFILE_VIVALDI_AVATAR_20, "monster_3.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_21},
      {IDR_PROFILE_VIVALDI_AVATAR_21, "monster_4.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_22},
      {IDR_PROFILE_VIVALDI_AVATAR_22, "monster_5.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_23},
      {IDR_PROFILE_VIVALDI_AVATAR_23, "monster_6.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_24},
      {IDR_PROFILE_VIVALDI_AVATAR_24, "monster_7.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_25},
      {IDR_PROFILE_VIVALDI_AVATAR_25, "monster_8.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_26},

      // Placeholder avatar icon:
      {IDR_PROFILE_VIVALDI_AVATAR_26, "avatar_generic.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_38},

#if !defined(OS_ANDROID)
      {IDR_PROFILE_VIVALDI_AVATAR_27, "monster_9.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_27},
      {IDR_PROFILE_VIVALDI_AVATAR_28, "monster_10.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_28},
      {IDR_PROFILE_VIVALDI_AVATAR_29, "avatar_animal_1.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_29},
      {IDR_PROFILE_VIVALDI_AVATAR_30, "avatar_animal_2.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_30},
      {IDR_PROFILE_VIVALDI_AVATAR_31, "avatar_animal_3.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_31},
      {IDR_PROFILE_VIVALDI_AVATAR_32, "avatar_animal_4.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_32},
      {IDR_PROFILE_VIVALDI_AVATAR_33, "avatar_animal_5.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_33},
      {IDR_PROFILE_VIVALDI_AVATAR_34, "avatar_animal_6.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_34},
      {IDR_PROFILE_VIVALDI_AVATAR_35, "avatar_animal_7.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_35},
      {IDR_PROFILE_VIVALDI_AVATAR_36, "avatar_animal_8.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_36},
      {IDR_PROFILE_VIVALDI_AVATAR_37, "avatar_animal_9.png",
       IDS_DEFAULT_VIVALDI_AVATAR_NAME_37},
#endif  // !defined(OS_ANDROID)
  };
#else
  static const IconResourceInfo resource_info[kDefaultAvatarIconsCount] = {
  // Old avatar icons:
#if !defined(OS_ANDROID)
    {IDR_PROFILE_AVATAR_0, "avatar_generic.png", IDS_DEFAULT_AVATAR_LABEL_0},
    {IDR_PROFILE_AVATAR_1, "avatar_generic_aqua.png",
     IDS_DEFAULT_AVATAR_LABEL_1},
    {IDR_PROFILE_AVATAR_2, "avatar_generic_blue.png",
     IDS_DEFAULT_AVATAR_LABEL_2},
    {IDR_PROFILE_AVATAR_3, "avatar_generic_green.png",
     IDS_DEFAULT_AVATAR_LABEL_3},
    {IDR_PROFILE_AVATAR_4, "avatar_generic_orange.png",
     IDS_DEFAULT_AVATAR_LABEL_4},
    {IDR_PROFILE_AVATAR_5, "avatar_generic_purple.png",
     IDS_DEFAULT_AVATAR_LABEL_5},
    {IDR_PROFILE_AVATAR_6, "avatar_generic_red.png",
     IDS_DEFAULT_AVATAR_LABEL_6},
    {IDR_PROFILE_AVATAR_7, "avatar_generic_yellow.png",
     IDS_DEFAULT_AVATAR_LABEL_7},
    {IDR_PROFILE_AVATAR_8, "avatar_secret_agent.png",
     IDS_DEFAULT_AVATAR_LABEL_8},
    {IDR_PROFILE_AVATAR_9, "avatar_superhero.png", IDS_DEFAULT_AVATAR_LABEL_9},
    {IDR_PROFILE_AVATAR_10, "avatar_volley_ball.png",
     IDS_DEFAULT_AVATAR_LABEL_10},
    {IDR_PROFILE_AVATAR_11, "avatar_businessman.png",
     IDS_DEFAULT_AVATAR_LABEL_11},
    {IDR_PROFILE_AVATAR_12, "avatar_ninja.png", IDS_DEFAULT_AVATAR_LABEL_12},
    {IDR_PROFILE_AVATAR_13, "avatar_alien.png", IDS_DEFAULT_AVATAR_LABEL_13},
    {IDR_PROFILE_AVATAR_14, "avatar_awesome.png", IDS_DEFAULT_AVATAR_LABEL_14},
    {IDR_PROFILE_AVATAR_15, "avatar_flower.png", IDS_DEFAULT_AVATAR_LABEL_15},
    {IDR_PROFILE_AVATAR_16, "avatar_pizza.png", IDS_DEFAULT_AVATAR_LABEL_16},
    {IDR_PROFILE_AVATAR_17, "avatar_soccer.png", IDS_DEFAULT_AVATAR_LABEL_17},
    {IDR_PROFILE_AVATAR_18, "avatar_burger.png", IDS_DEFAULT_AVATAR_LABEL_18},
    {IDR_PROFILE_AVATAR_19, "avatar_cat.png", IDS_DEFAULT_AVATAR_LABEL_19},
    {IDR_PROFILE_AVATAR_20, "avatar_cupcake.png", IDS_DEFAULT_AVATAR_LABEL_20},
    {IDR_PROFILE_AVATAR_21, "avatar_dog.png", IDS_DEFAULT_AVATAR_LABEL_21},
    {IDR_PROFILE_AVATAR_22, "avatar_horse.png", IDS_DEFAULT_AVATAR_LABEL_22},
    {IDR_PROFILE_AVATAR_23, "avatar_margarita.png",
     IDS_DEFAULT_AVATAR_LABEL_23},
    {IDR_PROFILE_AVATAR_24, "avatar_note.png", IDS_DEFAULT_AVATAR_LABEL_24},
    {IDR_PROFILE_AVATAR_25, "avatar_sun_cloud.png",
     IDS_DEFAULT_AVATAR_LABEL_25},
#endif
    // Placeholder avatar icon:
    {IDR_PROFILE_AVATAR_26, NULL, -1},

#if !defined(OS_CHROMEOS) && !defined(OS_ANDROID)
    // Modern avatar icons:
    {IDR_PROFILE_AVATAR_27, "avatar_origami_cat.png",
     IDS_DEFAULT_AVATAR_LABEL_27},
    {IDR_PROFILE_AVATAR_28, "avatar_origami_corgi.png",
     IDS_DEFAULT_AVATAR_LABEL_28},
    {IDR_PROFILE_AVATAR_29, "avatar_origami_dragon.png",
     IDS_DEFAULT_AVATAR_LABEL_29},
    {IDR_PROFILE_AVATAR_30, "avatar_origami_elephant.png",
     IDS_DEFAULT_AVATAR_LABEL_30},
    {IDR_PROFILE_AVATAR_31, "avatar_origami_fox.png",
     IDS_DEFAULT_AVATAR_LABEL_31},
    {IDR_PROFILE_AVATAR_32, "avatar_origami_monkey.png",
     IDS_DEFAULT_AVATAR_LABEL_32},
    {IDR_PROFILE_AVATAR_33, "avatar_origami_panda.png",
     IDS_DEFAULT_AVATAR_LABEL_33},
    {IDR_PROFILE_AVATAR_34, "avatar_origami_penguin.png",
     IDS_DEFAULT_AVATAR_LABEL_34},
    {IDR_PROFILE_AVATAR_35, "avatar_origami_pinkbutterfly.png",
     IDS_DEFAULT_AVATAR_LABEL_35},
    {IDR_PROFILE_AVATAR_36, "avatar_origami_rabbit.png",
     IDS_DEFAULT_AVATAR_LABEL_36},
    {IDR_PROFILE_AVATAR_37, "avatar_origami_unicorn.png",
     IDS_DEFAULT_AVATAR_LABEL_37},
    {IDR_PROFILE_AVATAR_38, "avatar_illustration_basketball.png",
     IDS_DEFAULT_AVATAR_LABEL_38},
    {IDR_PROFILE_AVATAR_39, "avatar_illustration_bike.png",
     IDS_DEFAULT_AVATAR_LABEL_39},
    {IDR_PROFILE_AVATAR_40, "avatar_illustration_bird.png",
     IDS_DEFAULT_AVATAR_LABEL_40},
    {IDR_PROFILE_AVATAR_41, "avatar_illustration_cheese.png",
     IDS_DEFAULT_AVATAR_LABEL_41},
    {IDR_PROFILE_AVATAR_42, "avatar_illustration_football.png",
     IDS_DEFAULT_AVATAR_LABEL_42},
    {IDR_PROFILE_AVATAR_43, "avatar_illustration_ramen.png",
     IDS_DEFAULT_AVATAR_LABEL_43},
    {IDR_PROFILE_AVATAR_44, "avatar_illustration_sunglasses.png",
     IDS_DEFAULT_AVATAR_LABEL_44},
    {IDR_PROFILE_AVATAR_45, "avatar_illustration_sushi.png",
     IDS_DEFAULT_AVATAR_LABEL_45},
    {IDR_PROFILE_AVATAR_46, "avatar_illustration_tamagotchi.png",
     IDS_DEFAULT_AVATAR_LABEL_46},
    {IDR_PROFILE_AVATAR_47, "avatar_illustration_vinyl.png",
     IDS_DEFAULT_AVATAR_LABEL_47},
    {IDR_PROFILE_AVATAR_48, "avatar_abstract_avocado.png",
     IDS_DEFAULT_AVATAR_LABEL_48},
    {IDR_PROFILE_AVATAR_49, "avatar_abstract_cappuccino.png",
     IDS_DEFAULT_AVATAR_LABEL_49},
    {IDR_PROFILE_AVATAR_50, "avatar_abstract_icecream.png",
     IDS_DEFAULT_AVATAR_LABEL_50},
    {IDR_PROFILE_AVATAR_51, "avatar_abstract_icewater.png",
     IDS_DEFAULT_AVATAR_LABEL_51},
    {IDR_PROFILE_AVATAR_52, "avatar_abstract_melon.png",
     IDS_DEFAULT_AVATAR_LABEL_52},
    {IDR_PROFILE_AVATAR_53, "avatar_abstract_onigiri.png",
     IDS_DEFAULT_AVATAR_LABEL_53},
    {IDR_PROFILE_AVATAR_54, "avatar_abstract_pizza.png",
     IDS_DEFAULT_AVATAR_LABEL_54},
    {IDR_PROFILE_AVATAR_55, "avatar_abstract_sandwich.png",
     IDS_DEFAULT_AVATAR_LABEL_55},
#endif
  };
#endif  // VIVALDI_BUILD
  return &resource_info[index];
}

int GetDefaultAvatarIconResourceIDAtIndex(size_t index) {
  return GetDefaultAvatarIconResourceInfo(index)->resource_id;
}

const char* GetDefaultAvatarIconFileNameAtIndex(size_t index) {
  CHECK_NE(index, kPlaceholderAvatarIndex);
  return GetDefaultAvatarIconResourceInfo(index)->filename;
}

base::FilePath GetPathOfHighResAvatarAtIndex(size_t index) {
  const char* file_name = GetDefaultAvatarIconFileNameAtIndex(index);
  base::FilePath user_data_dir;
  CHECK(base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir));
  return user_data_dir.AppendASCII(
      kHighResAvatarFolderName).AppendASCII(file_name);
}

std::string GetDefaultAvatarIconUrl(size_t index) {
#if !defined(OS_ANDROID)
  CHECK(IsDefaultAvatarIconIndex(index));
#endif
  return base::StringPrintf("%s%" PRIuS, kDefaultUrlPrefix, index);
}

int GetDefaultAvatarLabelResourceIDAtIndex(size_t index) {
  CHECK_NE(index, kPlaceholderAvatarIndex);
  return GetDefaultAvatarIconResourceInfo(index)->label_id;
}

bool IsDefaultAvatarIconIndex(size_t index) {
  return index < kDefaultAvatarIconsCount;
}

bool IsDefaultAvatarIconUrl(const std::string& url, size_t* icon_index) {
  DCHECK(icon_index);
#ifdef VIVALDI_BUILD
  // We need to handle the Chromium old urls
  if (base::StartsWith(url, kOldDefaultUrlPrefix, base::CompareCase::SENSITIVE)) {
    int int_value = -1;
    if (base::StringToInt(base::StringPiece(url.begin() +
      strlen(kOldDefaultUrlPrefix),
      url.end()),
      &int_value)) {
      if (int_value < 0 ||
        int_value >= static_cast<int>(kDefaultAvatarIconsCount))
        return false;
      *icon_index = int_value;
      return true;
    }
  }
#endif  // VIVALDI_BUILD
  if (!base::StartsWith(url, kDefaultUrlPrefix, base::CompareCase::SENSITIVE))
    return false;

  int int_value = -1;
  if (base::StringToInt(base::StringPiece(url.begin() +
                                          strlen(kDefaultUrlPrefix),
                                          url.end()),
                        &int_value)) {
    if (int_value < 0 ||
        int_value >= static_cast<int>(kDefaultAvatarIconsCount))
      return false;
    *icon_index = int_value;
    return true;
  }

  return false;
}

std::unique_ptr<base::ListValue> GetDefaultProfileAvatarIconsAndLabels(
    size_t selected_avatar_idx) {
  std::unique_ptr<base::ListValue> avatars(new base::ListValue());

#ifdef VIVALDI_BUILD
  for (size_t i = 0; i < kVivaldiAvatarsOnCreate; ++i) {
#else
  for (size_t i = GetModernAvatarIconStartIndex();
       i < GetDefaultAvatarIconCount(); ++i) {
#endif  // VIVALDI_BUILD
    std::unique_ptr<base::DictionaryValue> avatar_info(
        new base::DictionaryValue());
    avatar_info->SetString("url", profiles::GetDefaultAvatarIconUrl(i));
    avatar_info->SetString(
        "label", l10n_util::GetStringUTF16(
                     profiles::GetDefaultAvatarLabelResourceIDAtIndex(i)));
    if (i == selected_avatar_idx)
      avatar_info->SetBoolean("selected", true);

    avatars->Append(std::move(avatar_info));
  }
  return avatars;
}

size_t GetRandomAvatarIconIndex(
    const std::unordered_set<size_t>& used_icon_indices) {
  size_t interval_begin = GetModernAvatarIconStartIndex();
  size_t interval_end = GetDefaultAvatarIconCount();
  size_t interval_length = interval_end - interval_begin;

  size_t random_offset = base::RandInt(0, interval_length - 1);
  // Find the next unused index.
  for (size_t i = 0; i < interval_length; ++i) {
    size_t icon_index = interval_begin + (random_offset + i) % interval_length;
    if (used_icon_indices.count(icon_index) == 0u)
      return icon_index;
  }
  // All indices are used, so return a random one.
  return interval_begin + random_offset;
}

}  // namespace profiles
