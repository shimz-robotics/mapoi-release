// #297: map_state_store (last-selected map 永続化) の unit test。
// 契約: state file の**有無**が「初回起動 (nullopt) vs 運用中再起動 (has_value)」の判別軸。
// 値の中身 (restore 先) は二次情報で、読めない場合は empty string を返す。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "mapoi_server/map_state_store.hpp"

namespace fs = std::filesystem;

class MapStateStoreTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // test 名を含めて test 間の dir 衝突を避ける (binary 内は逐次実行だが残骸にも安全)。
    const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
    base_dir_ = fs::temp_directory_path() /
      (std::string("mapoi_state_store_test_") + info->name());
    fs::remove_all(base_dir_);
    fs::create_directories(base_dir_);
  }

  void TearDown() override {fs::remove_all(base_dir_);}

  fs::path state_file() const {return base_dir_ / mapoi::kLastMapStateFile;}

  fs::path base_dir_;
};

TEST_F(MapStateStoreTest, ReadReturnsNulloptWhenNoStateFile)
{
  // dir はあるが file が無い = 初回起動。
  EXPECT_FALSE(mapoi::read_last_map(base_dir_).has_value());
}

TEST_F(MapStateStoreTest, ReadReturnsNulloptWhenStateDirMissing)
{
  // dir ごと無い (state_path 初回指定) も初回起動扱い。
  EXPECT_FALSE(mapoi::read_last_map(base_dir_ / "not_created_yet").has_value());
}

TEST_F(MapStateStoreTest, WriteThenReadRoundtrip)
{
  std::string error;
  ASSERT_TRUE(mapoi::write_last_map(base_dir_, "map_b", error)) << error;
  const auto value = mapoi::read_last_map(base_dir_);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "map_b");
  // atomic write の tmp file が残らないこと。
  EXPECT_FALSE(fs::exists(base_dir_ / "last_selected_map.tmp"));
}

TEST_F(MapStateStoreTest, WriteCreatesStateDirRecursively)
{
  const auto nested = base_dir_ / "nested" / "state";
  std::string error;
  ASSERT_TRUE(mapoi::write_last_map(nested, "map_a", error)) << error;
  const auto value = mapoi::read_last_map(nested);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "map_a");
}

TEST_F(MapStateStoreTest, WriteOverwritesPreviousValue)
{
  std::string error;
  ASSERT_TRUE(mapoi::write_last_map(base_dir_, "map_a", error)) << error;
  ASSERT_TRUE(mapoi::write_last_map(base_dir_, "map_b", error)) << error;
  EXPECT_EQ(*mapoi::read_last_map(base_dir_), "map_b");
}

TEST_F(MapStateStoreTest, ReadTrimsWhitespaceAndUsesFirstLineOnly)
{
  // 手書き / エディタ経由の編集を想定: 前後空白と改行は無視、1 行目のみを map 名とする。
  std::ofstream ofs(state_file());
  ofs << "  map_a \t\n2 行目は無視される\n";
  ofs.close();
  const auto value = mapoi::read_last_map(base_dir_);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "map_a");
}

TEST_F(MapStateStoreTest, ReadEmptyFileReturnsEmptyValue)
{
  // 空 file: 「再起動」判定 (has_value) は維持しつつ restore 先は不明 (empty)。
  std::ofstream ofs(state_file());
  ofs.close();
  const auto value = mapoi::read_last_map(base_dir_);
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(value->empty());
}

TEST(IsPlainDirectoryNameTest, AcceptsPlainNames)
{
  EXPECT_TRUE(mapoi::is_plain_directory_name("turtlebot3_world"));
  EXPECT_TRUE(mapoi::is_plain_directory_name("map-01.backup"));
  EXPECT_TRUE(mapoi::is_plain_directory_name("..hidden"));  // ".." 単体でなければ可
}

TEST(IsPlainDirectoryNameTest, RejectsTraversalAndSeparators)
{
  // state file は破損・手動編集されうる外部入力。path segment として連結する前に
  // maps_path 外へ抜けられる値を弾く (PR #327 review medium)。
  EXPECT_FALSE(mapoi::is_plain_directory_name(""));
  EXPECT_FALSE(mapoi::is_plain_directory_name("."));
  EXPECT_FALSE(mapoi::is_plain_directory_name(".."));
  EXPECT_FALSE(mapoi::is_plain_directory_name("../other_maps"));
  EXPECT_FALSE(mapoi::is_plain_directory_name("a/b"));
  EXPECT_FALSE(mapoi::is_plain_directory_name("/etc"));
  EXPECT_FALSE(mapoi::is_plain_directory_name("..\\windows"));
  EXPECT_FALSE(mapoi::is_plain_directory_name("trailing/"));
}

TEST_F(MapStateStoreTest, WriteFailsWhenStateDirPathIsFile)
{
  // state_dir の位置に regular file がある誤設定: false + 理由メッセージを返す。
  const auto file_as_dir = base_dir_ / "occupied";
  std::ofstream ofs(file_as_dir);
  ofs << "x";
  ofs.close();
  std::string error;
  EXPECT_FALSE(mapoi::write_last_map(file_as_dir, "map_a", error));
  EXPECT_FALSE(error.empty());
}
