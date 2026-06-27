#include "openxmb/xmb/layout.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

import openxmb.xmb.root_scene;

namespace {

using namespace openxmb::xmb;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "live crossbar contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void require_near(double actual, double expected, std::string_view message) {
  require(std::abs(actual - expected) < 0.0001, message);
}

void test_root_order_and_assets() {
  constexpr std::array labels{
      std::string_view{"Users"},
      std::string_view{"Settings"},
      std::string_view{"Photo"},
      std::string_view{"Music"},
      std::string_view{"Video"},
      std::string_view{"Game"},
      std::string_view{"Network"},
      std::string_view{"PlayStation Network"},
      std::string_view{"Friends"},
  };
  require(kRootCategories.size() == labels.size(), "root has exactly nine categories");
  for (std::size_t index = 0; index < labels.size(); ++index) {
    require(kRootCategories[index].label == labels[index], "root category order");
  }

  require(kUsersItems.size() == 3, "Users root has exactly three entries");
  require(kUsersItems[0].label == "Turn Off System", "Turn Off System is first");
  require(kUsersItems[0].icon_semantic_id == "xmb.icon.054", "Turn Off System icon");
  require(kUsersItems[1].label == "Create New User", "Create New User is second");
  require(kUsersItems[1].icon_semantic_id == "xmb.icon.041", "Create New User icon");
  require(kUsersItems[2].label == "*User", "current user is third");
  require(kUsersItems[2].icon_semantic_id == "xmb.icon.041", "current user icon");
}

void test_measured_geometry_and_timing() {
  auto state = make_users_root_scene();
  const auto initial = sample_root_scene(state, 0.0);
  require_near(initial.categories[0].center.x, 566.0, "active category x");
  require_near(initial.categories[0].center.y, 279.0, "active category y");
  require_near(initial.categories[0].icon_extent, 168.0, "active category extent");
  require_near(initial.categories[1].center.x, 769.0, "category pitch");
  require_near(initial.categories[1].center.y, 289.0, "inactive category baseline");
  require_near(initial.categories[1].icon_extent, 117.0, "inactive category extent");
  require_near(initial.categories[0].label_y, 342.0, "category label y");
  require_near(initial.categories[0].label_size, 20.0, "category label size");

  require_near(initial.users_items[0].icon_center.y, 515.0, "focused Users y");
  require_near(initial.users_items[0].icon_extent, 180.0, "focused Users extent");
  require_near(initial.users_items[0].label_anchor.x, 683.0, "focused Users label x");
  require_near(initial.users_items[0].label_size, 34.0, "focused Users label size");
  require_near(initial.users_items[1].icon_center.y, 682.0, "second Users y");
  require_near(initial.users_items[2].icon_center.y, 762.0, "third Users y");

  require(select_root_category(state, 1, 10.0), "category transition starts");
  const auto category_halfway = sample_root_scene(state, 10.125);
  require_near(category_halfway.category_transition_progress, 0.5,
               "category transition is 250 ms");

  state = make_users_root_scene();
  require(select_users_item(state, 1, 20.0), "item transition starts");
  const auto item_halfway = sample_root_scene(state, 20.1);
  require_near(item_halfway.item_transition_progress, 0.5,
               "item transition is 200 ms");
}

void test_contained_aspects() {
  const auto four_three = make_layout_transform({1440, 1080});
  require_near(four_three.scale_x, 0.75, "4:3 contain scale");
  require_near(four_three.offset_x, 0.0, "4:3 horizontal origin");
  require_near(four_three.offset_y, 135.0, "4:3 letterbox offset");
  const auto four_three_focus = four_three.point_to_framebuffer({566.0, 515.0});
  require_near(four_three_focus.x, 424.5, "4:3 focus x");
  require_near(four_three_focus.y, 521.25, "4:3 focus y");

  const auto ultra_wide = make_layout_transform({2560, 1080});
  require_near(ultra_wide.scale_x, 1.0, "21:9 contain scale");
  require_near(ultra_wide.offset_x, 320.0, "21:9 pillarbox offset");
  require_near(ultra_wide.offset_y, 0.0, "21:9 vertical origin");
}

} // namespace

int main() {
  test_root_order_and_assets();
  test_measured_geometry_and_timing();
  test_contained_aspects();
  std::cout << "live crossbar contract tests passed\n";
}
