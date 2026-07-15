#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "mapoi_server/system_tags.hpp"

namespace
{

std::vector<std::string> load_system_tag_names()
{
  std::vector<std::string> names;
  for (const auto & tag : mapoi::kSystemTags) {
    names.push_back(tag.name);
  }
  return names;
}

}  // namespace

TEST(SystemTagsContract, DefinesExpectedSystemTagsInStableOrder)
{
  const std::vector<std::string> expected = {"waypoint", "landmark", "pause"};
  EXPECT_EQ(load_system_tag_names(), expected);
}

TEST(SystemTagsContract, NamesAreUniqueAndNonEmpty)
{
  const auto names = load_system_tag_names();
  std::set<std::string> unique_names(names.begin(), names.end());

  EXPECT_EQ(unique_names.size(), names.size());
  for (const auto & name : names) {
    EXPECT_FALSE(name.empty());
  }
}

TEST(SystemTagsContract, DescriptionsArePresent)
{
  for (const auto & tag : mapoi::kSystemTags) {
    ASSERT_NE(tag.name, nullptr);
    ASSERT_NE(tag.description, nullptr);
    EXPECT_GT(std::strlen(tag.description), 0u)
      << "system tag '" << tag.name << "' description is empty";
  }
}
