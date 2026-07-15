#include "mapoi_server/map_state_store.hpp"

#include <fstream>
#include <system_error>

namespace mapoi
{

namespace
{

std::string trim(const std::string & s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

}  // namespace

std::optional<std::string> read_last_map(const std::filesystem::path & state_dir)
{
  const auto state_file = state_dir / kLastMapStateFile;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(state_file, ec) || ec) {
    // file 無し (or stat 失敗): 判別不能なケースも安全側ではなく「初回起動」に倒す。
    // 誤って「再起動」扱いにすると初回起動の #144 自動 initial pose を失うため。
    return std::nullopt;
  }
  std::ifstream ifs(state_file);
  if (!ifs) {
    // 存在するのに開けない: 「運用中再起動」判定は維持しつつ restore 先は不明を返す。
    return std::string{};
  }
  std::string line;
  std::getline(ifs, line);
  return trim(line);
}

bool is_plain_directory_name(const std::string & name)
{
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  // '\\' は POSIX ではファイル名に使える文字だが、Windows separator との両対応と
  // 防御的一貫性のため一律拒否する。
  return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

bool write_last_map(
  const std::filesystem::path & state_dir, const std::string & map_name,
  std::string & error_message)
{
  std::error_code ec;
  std::filesystem::create_directories(state_dir, ec);
  if (ec) {
    error_message =
      "failed to create state dir '" + state_dir.string() + "': " + ec.message();
    return false;
  }
  const auto state_file = state_dir / kLastMapStateFile;
  const auto tmp_file = state_dir / (std::string(kLastMapStateFile) + ".tmp");
  {
    std::ofstream ofs(tmp_file, std::ios::trunc);
    if (!ofs) {
      error_message = "failed to open '" + tmp_file.string() + "' for writing";
      return false;
    }
    ofs << map_name << '\n';
    ofs.flush();
    if (!ofs) {
      error_message = "failed to write '" + tmp_file.string() + "'";
      return false;
    }
  }
  std::filesystem::rename(tmp_file, state_file, ec);
  if (ec) {
    error_message = "failed to rename '" + tmp_file.string() + "' -> '" +
      state_file.string() + "': " + ec.message();
    return false;
  }
  return true;
}

}  // namespace mapoi
