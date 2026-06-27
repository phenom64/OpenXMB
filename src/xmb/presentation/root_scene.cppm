module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

export module openxmb.xmb.root_scene;

export namespace openxmb::xmb {

inline constexpr double kCategoryBaselineY = 289.0;
inline constexpr double kActiveCategoryX = 566.0;
inline constexpr double kCategorySpacing = 203.0;
inline constexpr double kActiveCategoryIconExtent = 168.0;
inline constexpr double kInactiveCategoryIconExtent = 117.0;
inline constexpr double kCategoryLabelY = 342.0;
inline constexpr double kCategoryLabelSize = 20.0;
inline constexpr double kItemFocusY = 515.0;
inline constexpr double kItemPitch = 80.0;
inline constexpr double kSelectedToNextPadding = 87.0;
inline constexpr double kItemIconX = 566.0;
inline constexpr double kInactiveItemIconExtent = 78.0;
inline constexpr double kActiveItemIconExtent = 180.0;
inline constexpr double kItemLabelX = 683.0;
inline constexpr double kInactiveItemLabelSize = 30.0;
inline constexpr double kActiveItemLabelSize = 34.0;
inline constexpr double kActiveItemScaleTier = 1.3;
inline constexpr double kInactiveItemScaleTier = 0.833;
inline constexpr double kTweenUpScaleTier = 1.06;
inline constexpr double kTweenDownScaleTier = 0.76;
inline constexpr double kFocusAlpha = 1.0;
inline constexpr double kInactiveItemAlpha = 0.48;
inline constexpr double kNearAlpha = 0.8;
inline constexpr double kMiddleAlpha = 0.5;
inline constexpr double kFarAlpha = 0.3;
inline constexpr double kCategoryTransitionSeconds = 0.250;
inline constexpr double kItemTransitionSeconds = 0.200;

enum class RootCategoryId : std::uint8_t {
  users,
  settings,
  photo,
  music,
  video,
  game,
  network,
  playstation_network,
  friends,
};

enum class UsersItemId : std::uint8_t {
  turn_off_system,
  create_new_user,
  current_user,
};

struct RootCategoryDefinition {
  RootCategoryId id{};
  std::string_view semantic_id;
  std::string_view label;
  std::string_view icon_semantic_id;
};

struct UsersItemDefinition {
  UsersItemId id{};
  std::string_view semantic_id;
  std::string_view label;
  std::string_view icon_semantic_id;
};

inline constexpr std::array<RootCategoryDefinition, 9> kRootCategories{{
    {RootCategoryId::users, "root.category.users", "Users",
     "xmb.icon.category.users"},
    {RootCategoryId::settings, "root.category.settings", "Settings",
     "xmb.icon.category.settings"},
    {RootCategoryId::photo, "root.category.photo", "Photo",
     "xmb.icon.category.photo"},
    {RootCategoryId::music, "root.category.music", "Music",
     "xmb.icon.category.music"},
    {RootCategoryId::video, "root.category.video", "Video",
     "xmb.icon.category.video"},
    {RootCategoryId::game, "root.category.game", "Game",
     "xmb.icon.category.game"},
    {RootCategoryId::network, "root.category.network", "Network",
     "xmb.icon.category.network"},
    {RootCategoryId::playstation_network, "root.category.playstation-network",
     "PlayStation Network", "xmb.icon.category.playstation-network"},
    {RootCategoryId::friends, "root.category.friends", "Friends",
     "xmb.icon.category.friends"},
}};

inline constexpr std::array<UsersItemDefinition, 3> kUsersItems{{
    {UsersItemId::turn_off_system, "users.item.turn-off-system",
     "Turn Off System", "xmb.icon.054"},
    {UsersItemId::create_new_user, "users.item.create-new-user",
     "Create New User", "xmb.icon.041"},
    {UsersItemId::current_user, "users.item.current-user", "*User",
     "xmb.icon.041"},
}};

struct RootSceneState {
  std::size_t active_category{};
  std::size_t previous_category{};
  double category_transition_started_seconds{};

  std::size_t selected_users_item{};
  std::size_t previous_users_item{};
  double item_transition_started_seconds{};
};

struct RootReveal {
  double ui{1.0};
  double label{1.0};
  double icons{1.0};
};

struct ScenePoint {
  double x{};
  double y{};

  [[nodiscard]] constexpr bool
  operator==(const ScenePoint &) const noexcept = default;
};

struct CategoryVisual {
  RootCategoryDefinition definition{};
  ScenePoint center{};
  double icon_extent{};
  double alpha{};
  double label_y{kCategoryLabelY};
  double label_size{kCategoryLabelSize};
  double label_alpha{};
  bool active{};
};

struct UsersItemVisual {
  UsersItemDefinition definition{};
  ScenePoint icon_center{};
  ScenePoint label_anchor{};
  double icon_extent{};
  double label_size{};
  double alpha{};
  double scale_tier{};
  bool focused{};
};

struct RootSceneSnapshot {
  std::array<CategoryVisual, kRootCategories.size()> categories{};
  std::array<UsersItemVisual, kUsersItems.size()> users_items{};
  double category_transition_progress{1.0};
  double item_transition_progress{1.0};
};

[[nodiscard]] RootSceneState make_users_root_scene() noexcept;
[[nodiscard]] bool
select_root_category(RootSceneState &state, std::size_t category_index,
                     double transition_started_seconds) noexcept;
[[nodiscard]] bool
select_users_item(RootSceneState &state, std::size_t item_index,
                  double transition_started_seconds) noexcept;
[[nodiscard]] RootSceneSnapshot
sample_root_scene(const RootSceneState &state, double now_seconds,
                  RootReveal reveal = {}) noexcept;

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

[[nodiscard]] constexpr double saturate(double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] constexpr double ease_out_cubic(double value) noexcept {
  const auto t = saturate(value);
  const auto inverse = 1.0 - t;
  return 1.0 - inverse * inverse * inverse;
}

[[nodiscard]] constexpr double smooth(double value) noexcept {
  const auto t = saturate(value);
  return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] constexpr double item_slot_y(std::size_t item,
                                           std::size_t selected) noexcept {
  if (item == selected) {
    return kItemFocusY;
  }
  if (item < selected) {
    constexpr auto above_base_y = 120.0;
    return above_base_y - static_cast<double>(selected - item - 1) * kItemPitch;
  }
  return kItemFocusY + kSelectedToNextPadding +
         static_cast<double>(item - selected) * kItemPitch;
}

[[nodiscard]] constexpr double interpolate(double from, double to,
                                           double progress) noexcept {
  return from + (to - from) * progress;
}

} // namespace

RootSceneState make_users_root_scene() noexcept { return {}; }

bool select_root_category(RootSceneState &state, std::size_t category_index,
                          double transition_started_seconds) noexcept {
  if (category_index >= kRootCategories.size() ||
      category_index == state.active_category) {
    return false;
  }
  state.previous_category = state.active_category;
  state.active_category = category_index;
  state.category_transition_started_seconds = transition_started_seconds;
  return true;
}

bool select_users_item(RootSceneState &state, std::size_t item_index,
                       double transition_started_seconds) noexcept {
  if (item_index >= kUsersItems.size() ||
      item_index == state.selected_users_item) {
    return false;
  }
  state.previous_users_item = state.selected_users_item;
  state.selected_users_item = item_index;
  state.item_transition_started_seconds = transition_started_seconds;
  return true;
}

RootSceneSnapshot sample_root_scene(const RootSceneState &state,
                                    double now_seconds,
                                    RootReveal reveal) noexcept {
  RootSceneSnapshot result{};

  const auto category_linear =
      state.active_category == state.previous_category
          ? 1.0
          : saturate((now_seconds - state.category_transition_started_seconds) /
                     kCategoryTransitionSeconds);
  const auto item_linear =
      state.selected_users_item == state.previous_users_item
          ? 1.0
          : saturate((now_seconds - state.item_transition_started_seconds) /
                     kItemTransitionSeconds);
  result.category_transition_progress = category_linear;
  result.item_transition_progress = item_linear;

  const auto category_eased = ease_out_cubic(category_linear);
  const auto item_eased = ease_out_cubic(item_linear);
  const auto visual_category =
      interpolate(static_cast<double>(state.previous_category),
                  static_cast<double>(state.active_category), category_eased);

  reveal.ui = saturate(reveal.ui);
  reveal.label = saturate(reveal.label);
  reveal.icons = saturate(reveal.icons);

  for (std::size_t index = 0; index < kRootCategories.size(); ++index) {
    const auto active = index == state.active_category;
    const auto distance = index > state.active_category
                              ? index - state.active_category
                              : state.active_category - index;
    const auto base_alpha = active ? 1.0 : (distance <= 2 ? 0.9 : 0.25);

    auto icon_reveal = reveal.icons;
    if (icon_reveal < 0.999) {
      const auto stagger = std::min(1.0, static_cast<double>(distance) * 0.18);
      icon_reveal =
          smooth((icon_reveal - stagger) / std::max(0.001, 1.0 - stagger));
    }

    result.categories[index] = {
        .definition = kRootCategories[index],
        .center =
            {
                kActiveCategoryX +
                    (static_cast<double>(index) - visual_category) *
                        kCategorySpacing,
                kCategoryBaselineY + (active ? -10.0 : 0.0) +
                    (1.0 - icon_reveal) * 14.0,
            },
        .icon_extent =
            active ? kActiveCategoryIconExtent : kInactiveCategoryIconExtent,
        .alpha = base_alpha * icon_reveal,
        .label_y = kCategoryLabelY,
        .label_size = kCategoryLabelSize,
        .label_alpha = active ? 0.9 * reveal.label : 0.0,
        .active = active,
    };
  }

  for (std::size_t index = 0; index < kUsersItems.size(); ++index) {
    const auto focused = index == state.selected_users_item;
    const auto old_y = item_slot_y(index, state.previous_users_item);
    const auto new_y = item_slot_y(index, state.selected_users_item);
    const auto y = interpolate(old_y, new_y, item_eased);
    const auto alpha =
        (focused ? kFocusAlpha : kInactiveItemAlpha) * reveal.ui * reveal.icons;

    result.users_items[index] = {
        .definition = kUsersItems[index],
        .icon_center = {kItemIconX, y},
        .label_anchor = {kItemLabelX, y},
        .icon_extent =
            focused ? kActiveItemIconExtent : kInactiveItemIconExtent,
        .label_size = focused ? kActiveItemLabelSize : kInactiveItemLabelSize,
        .alpha = alpha,
        .scale_tier = focused ? kActiveItemScaleTier : kInactiveItemScaleTier,
        .focused = focused,
    };
  }

  return result;
}

} // namespace openxmb::xmb
