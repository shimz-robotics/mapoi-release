# syntax=docker/dockerfile:1.6
ARG ROS_DISTRO=jazzy

FROM osrf/ros:${ROS_DISTRO}-desktop-full AS base

ARG ROS_DISTRO
ENV ROS_DISTRO=${ROS_DISTRO}
ENV DEBIAN_FRONTEND=noninteractive

# 共通 apt 依存
# ros-${ROS_DISTRO}-rmw-cyclonedds-cpp: container は host の RMW_IMPLEMENTATION を継承する
# (docker-compose.yml の environment 参照)。host が cyclonedds を使っている場合に
# librmw_cyclonedds_cpp.so がないと ROS node 起動失敗するため、両 RMW を image に含める。
# fastrtps は ros-${ROS_DISTRO}-desktop-full に既定で含まれているので追加不要。
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-pip \
    python3-colcon-common-extensions \
    python3-rosdep \
    python3-flask \
    python3-pil \
    python3-yaml \
    ros-${ROS_DISTRO}-rmw-cyclonedds-cpp \
    ros-${ROS_DISTRO}-turtlebot3 \
    ros-${ROS_DISTRO}-turtlebot3-gazebo \
    ros-${ROS_DISTRO}-turtlebot3-navigation2 \
    fonts-noto-cjk \
 && rm -rf /var/lib/apt/lists/*

# 非 root ユーザー（UID/GID はビルド引数でホストと合わせる）
ARG USER_NAME=ros
ARG USER_ID=1000
ARG GROUP_ID=1000
# Ubuntu 24.04 (Jazzy ベース) には UID 1000 の ubuntu ユーザーが pre-created
# されており、そのまま useradd -u 1000 すると exit 4 (UID in use) で落ちる。
# 衝突する UID/GID を占有する既存ユーザー・グループを先に削除してから作り直す。
# GID だけが衝突するケース (e.g. USER_ID=1001, GROUP_ID=1000) でも
# 既存グループを使っているユーザーを userdel しないと groupdel が失敗し、
# useradd -g が元の競合グループにぶら下がって primary group の GID が
# ずれる事故が起きるため、両方を明示的にクリアする。
RUN existing_user_by_uid="$(getent passwd ${USER_ID} | cut -d: -f1)" \
 && if [ -n "${existing_user_by_uid}" ] && [ "${existing_user_by_uid}" != "${USER_NAME}" ]; then \
      userdel -r "${existing_user_by_uid}" 2>/dev/null || userdel "${existing_user_by_uid}" ; \
    fi \
 && existing_user_by_gid="$(getent passwd | awk -F: -v g=${GROUP_ID} '$4==g {print $1; exit}')" \
 && if [ -n "${existing_user_by_gid}" ] && [ "${existing_user_by_gid}" != "${USER_NAME}" ]; then \
      userdel -r "${existing_user_by_gid}" 2>/dev/null || userdel "${existing_user_by_gid}" ; \
    fi \
 && existing_group="$(getent group ${GROUP_ID} | cut -d: -f1)" \
 && if [ -n "${existing_group}" ] && [ "${existing_group}" != "${USER_NAME}" ]; then \
      groupdel "${existing_group}" ; \
    fi \
 && groupadd -g ${GROUP_ID} ${USER_NAME} \
 && useradd -m -u ${USER_ID} -g ${GROUP_ID} -s /bin/bash ${USER_NAME} \
 && [ "$(getent group ${GROUP_ID} | cut -d: -f1)" = "${USER_NAME}" ] \
 && echo "source /opt/ros/${ROS_DISTRO}/setup.bash" >> /home/${USER_NAME}/.bashrc \
 && echo "[ -f /ros2_ws/install/setup.bash ] && source /ros2_ws/install/setup.bash" >> /home/${USER_NAME}/.bashrc \
 && echo "export TURTLEBOT3_MODEL=burger" >> /home/${USER_NAME}/.bashrc

ENV WS=/ros2_ws
ENV TURTLEBOT3_MODEL=burger
RUN mkdir -p ${WS}/src && chown -R ${USER_NAME}:${USER_NAME} ${WS}
WORKDIR ${WS}

# Gazebo デフォルトモデル (ground_plane, sun) を vendor 済みファイルから配置する。
# docker-compose.yml で GAZEBO_MODEL_DATABASE_URI= を空にしてオンライン fetch を
# 止めているため、これがないと turtlebot3_world.world が ground_plane をロード
# できず、スポーンしたロボットが床下に落下する。
# 由来と更新方法は mapoi_turtlebot3_example/gazebo_models/README.md を参照。
COPY --chown=${USER_NAME}:${USER_NAME} mapoi_turtlebot3_example/gazebo_models /home/${USER_NAME}/.gazebo/models

# ----- dev stage: bind mount 前提、開発用 -----
FROM base AS dev
ARG USER_NAME=ros
RUN apt-get update && apt-get install -y --no-install-recommends \
    vim less git gdb tmux \
 && rm -rf /var/lib/apt/lists/*
USER ${USER_NAME}
CMD ["bash"]

# ----- runtime stage: ソース焼き込み、お試し/デモ用 -----
FROM base AS runtime
ARG ROS_DISTRO
ARG USER_NAME=ros
COPY --chown=${USER_NAME}:${USER_NAME} . ${WS}/src/mapoi/
USER ${USER_NAME}
RUN bash -lc "source /opt/ros/${ROS_DISTRO}/setup.bash \
 && cd ${WS} \
 && rosdep update --rosdistro ${ROS_DISTRO} \
 && rosdep install --from-paths src --ignore-src -r -y \
 && colcon build --symlink-install"

CMD ["bash", "-lc", "source /opt/ros/${ROS_DISTRO}/setup.bash && source /ros2_ws/install/setup.bash && ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml"]
