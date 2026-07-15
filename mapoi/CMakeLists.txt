# metapackage: 実体は package.xml の exec_depend のみ (navigation2 等と同じ構成)。
# リポジトリルート直下に package.xml を置くと colcon がそこで探索を打ち切り
# 配下 5 パッケージを発見できなくなるため、サブディレクトリに置く (#348)。
cmake_minimum_required(VERSION 3.8)
project(mapoi)
find_package(ament_cmake REQUIRED)
ament_package()
