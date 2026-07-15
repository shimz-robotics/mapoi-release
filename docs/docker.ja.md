# Docker で試す（demo）

> English version (primary): [docker.md](./docker.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

Docker イメージは **demo / 動作確認用**です。自作ロボットに組み込む場合は [自分のロボットへの導入方法](./integration.ja.md) を参照してください（将来 apt/rosdep での提供を予定しています）。

Linux ホストが前提で、Turtlebot3 サンプルを Gazebo / RViz2 / Web UI で体験できます。

## 前提

- Docker Engine + Docker Compose v2（ソースからビルドする場合）
- Linux ホスト（X11 経由で GUI 表示）
- GPU は不要（CPU レンダリングで動作）

## ビルド済みイメージから試す（最速）

ghcr.io に公開されたイメージを pull するだけで起動できます。

### Jazzy (gz-sim、推奨)

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY \
  -e QT_X11_NO_MITSHM=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy
```

### Humble (Gazebo Classic、Classic は EoL 2025-01)

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY \
  -e QT_X11_NO_MITSHM=1 \
  -e GAZEBO_MODEL_DATABASE_URI= \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:humble
```

Nav2 lifecycle 立ち上げに 30〜60 秒ほどかかるため、起動直後の信号受信で kill せず、少し待ってから Web UI にアクセスしてください。

起動後、ブラウザで http://localhost:8765 にアクセスすると Web UI が表示されます。

X11 接続が拒否される場合は `xhost +local:` を試してください（ローカル unix socket 経由の全接続を許可）。配布イメージ内のユーザーは UID 1000 固定のため、`$HOME/.Xauthority` の bind mount は行わず、X11 アクセス許可は `xhost` に委ねています。

> **片付け**: `xhost +local:docker` / `xhost +local:` は X server へのアクセスを広く許可するため、demo 終了後は `xhost -local:docker` (個別解除) または `xhost -` (全解除) で元に戻すと安全です。

利用可能なタグ:

| タグ | 説明 |
| --- | --- |
| `latest` | 推奨版 (`jazzy` と同一、primary distro) |
| `jazzy` | ROS 2 Jazzy + gz-sim (Gazebo Harmonic)、primary distro |
| `humble` | ROS 2 Humble + Gazebo Classic、Humble 環境向け継続提供 (Gazebo Classic は 2025-01 に EoL 済、ROS 2 Humble 自体は 2027-05 まで。新規プロジェクトでは Jazzy + gz-sim 推奨) |
| `vX.Y.Z` | リリースタグ版 (primary distro: jazzy) |
| `vX.Y.Z-jazzy` | リリースタグ版の jazzy ビルド |
| `vX.Y.Z-humble` | リリースタグ版の humble ビルド |

中身の ROS 2 バージョンは `docker inspect ghcr.io/shimz-robotics/mapoi:<タグ> | grep ROS_DISTRO` で確認できます。

## トラブルシューティング

### Web UI が「Navigation unavailable」のまま

WebUI の Navigation バッジは nav バックエンド (Nav2 action server + mapoi) の readiness を表示します。次の順に確認してください。

1. **起動直後は待つ**: Nav2 lifecycle の activate に 30〜60 秒かかります。その間 unavailable 表示は正常で、揃うと「Navigation connected」に変わります。
2. **イメージを最新化する**: `jazzy` / `latest` は main 追従のローリングタグで、`docker run` は手元のキャッシュを黙って使います。以前 pull した古いイメージを掴んでいないか `docker pull ghcr.io/shimz-robotics/mapoi:jazzy` で更新してください。再現性が要る検証では release tag (`vX.Y.Z`) の利用も検討してください。
3. **GUI (display) 不調は nav を止めません**: `docker logs <container>` に `could not connect to display` / `qt.qpa.xcb` が出ていれば、X へ接続できず RViz / Gazebo の GUI が起動できていません。GUI が起動できなくても nav バックエンドと WebUI は動作します (この挙動は #295 以降のイメージが前提。それより古いキャッシュのままだと GUI crash で launch 全体が停止する旧挙動のため、手順 2 の `docker pull` で更新してください)。RViz / Gazebo ウィンドウも表示したい場合は、起動前に `xhost +local:docker` を実行してください (詳細は上の「ビルド済みイメージから試す」節)。
4. **WebUI だけ使う (GUI 不要)**: GUI を起動せず WebUI / nav だけ動かすには headless で起動します。

   ```sh
   docker run --rm -it --network host --ipc host \
     ghcr.io/shimz-robotics/mapoi:jazzy \
     bash -lc "source /opt/ros/\${ROS_DISTRO}/setup.bash && source /ros2_ws/install/setup.bash && \
       ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml gazebo_gui:=false rviz:=false"
   ```

## ソースから docker compose build（開発者向け）

ローカル変更を含めて試したい場合は、リポジトリを clone して compose でビルドします。

> **`ROS_DISTRO` 環境変数の挙動**: `docker compose` の image / build は `${ROS_DISTRO:-jazzy}` で distro を切り替えます。
> - shell / `.env` 由来の `ROS_DISTRO` が **非空** ならその値を採用 (例: host で ROS 2 Humble を source 済みなら `humble`、Jazzy なら `jazzy`)
> - **空または未設定** なら fallback default は `jazzy` (primary distro)
> - **別 distro を一時的に試す** には `ROS_DISTRO=humble docker compose ...` のように前置で上書き (親 shell の `ROS_DISTRO` には影響しない)
> - local image は `mapoi:<service>-${ROS_DISTRO}` の形 (例: `mapoi:dev-jazzy` / `mapoi:demo-humble`) で distro 別保存、並行運用可
> - 詳細: [Docker Compose interpolation](https://docs.docker.com/reference/compose-file/interpolation/)

> **DDS 環境の扱い**: container 内 DDS 環境変数は以下の方針で設定されます:
> - **`RMW_IMPLEMENTATION`**: shell の `${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}` で host 値を **継承** (未設定なら `rmw_fastrtps_cpp` fallback)。host と一致しないと `network_mode: host` + `ipc: host` 越しに container <-> host の DDS discovery が成立しないため、host が cyclonedds なら container も cyclonedds に揃える
> - **`ROS_DOMAIN_ID`**: 同様に `${ROS_DOMAIN_ID:-0}` で host 継承、未設定なら 0。multi-robot / parallel simulation で domain 分離している運用も尊重
> - **`CYCLONEDDS_URI`**: 常に **空で固定** (host の custom URI を継承させない)。host で別 workspace の `CYCLONEDDS_URI=/path/to/custom.xml` を export していても、container 内では cyclonedds default config で動作 (これが過去 SwitchMap / discovery 障害の原因だった事例 → blocking)
> - Override したい場合 (例: 別 RMW で試す、別 domain 指定):
>   - `ROS_DISTRO=humble docker compose run --rm --env ROS_DOMAIN_ID=42 dev` で個別 override
>   - host の cyclonedds custom config を container でも使いたい (rare):
>     `ROS_DISTRO=humble docker compose run --rm --env CYCLONEDDS_URI=file:///path/to/cyclonedds.xml dev`
> - **注意**: `docker compose up` では `--env` flag は使えないため、`up` 経由で override が必要なら `docker-compose.yml` 編集または compose override file を使う

```sh
git clone https://github.com/shimz-robotics/mapoi.git
cd mapoi
xhost +local:docker
docker compose up demo
```

起動後、ブラウザで http://localhost:8765 にアクセスすると Web UI が表示されます。

## 開発用（ホストのソースを bind mount、コンテナ内でビルド）

```sh
# 初回ビルド
docker compose build dev

# シェルに入る（ホストの ./ が /ros2_ws/src/mapoi にマウント済み、--name で
# 後続の別 terminal が docker exec で入れるよう名前を付ける）
docker compose run --rm --name mapoi-dev dev

# コンテナ内
cd /ros2_ws
rosdep update --rosdistro ${ROS_DISTRO}
rosdep install --from-paths src --ignore-src -r -y
colcon build --symlink-install
source install/setup.bash
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml
```

ホスト側の `git` / VSCode でソース編集し、コンテナ内で再ビルドして反復できます。

### 複数 terminal で同じ container に入る

`ros2 topic echo` / `ros2 topic pub` を別 terminal から流したい場合、`docker compose run --rm dev` を別 terminal で再実行すると **新しい container** が立ち上がり、`/ros2_ws/install/` (= 1 つ目で `colcon build` した出力) が無いため `ros2 topic echo` で msg 型解決に失敗する (`mapoi_interfaces/msg/PoiEvent` 等が未知)。同 container に入るには `docker exec` を使う:

```sh
# 1 つ目の terminal で起動済み container に入る
docker exec -it mapoi-dev bash
# container 内 (1 つ目の install/ をそのまま参照)
source /ros2_ws/install/setup.bash
ros2 topic echo /mapoi/events
```

`network_mode: host` 越しに別 container からも DDS 通信は通るが、msg 型の install dir は container 固有なので、観測する側に同じ install dir が無いと echo が出力できない点に注意。

#### `Container mapoi-dev Error response from daemon: Conflict ... name "/mapoi-dev" is already in use` が出た場合

直前の `docker compose run --rm --name mapoi-dev dev` を Ctrl-C で抜けた場合等、停止済の container `mapoi-dev` が orphan として残ることがあります。次の `run` で同名衝突するので、再起動前に削除します:

```sh
docker rm mapoi-dev    # 停止済の container を削除
ROS_DISTRO=jazzy docker compose run --rm --name mapoi-dev dev    # 改めて起動
```

`--rm` flag は通常の exit (Ctrl-D / `exit`) では消える挙動ですが、Ctrl-C や強制 kill の経路では残るパターンがあります。

## 権限（UID/GID）の調整

container 内 `ros` user (default UID 1000) と host user の UID/GID が一致しないと、bind mount (`./:/ros2_ws/src/mapoi`) 越しの **書き込み操作で permission denied** が発生します。代表ケース:

- POI Editor Panel の **Save** クリック (mapoi_config.yaml に書き込み)
- container 内 `colcon build` の install/build/log 出力 (host にも残る場合)
- container 内で source ファイルを編集

**host UID が 1000 と異なる場合は必須** の手順 (UID 1000 の host では default のまま動作):

```sh
# 推奨: .env を生成して docker compose に自動 inject (.gitignore + .dockerignore 済)
echo "USER_ID=$(id -u)"   > .env
echo "GROUP_ID=$(id -g)" >> .env
docker compose build dev    # .env が自動で読み込まれる

# あるいは都度 prefix:
USER_ID=$(id -u) GROUP_ID=$(id -g) docker compose build dev
```

`.env` 作成後は build / run / up すべての compose コマンドで自動的に host UID が反映されます。各 key の意味とコメントは [.env.example](../.env.example) を参照。distro 別 image (mapoi:dev-${ROS_DISTRO}) ごとに UID は build 時に焼き込まれるため、UID 変更時は対象 distro の image を rebuild してください。

## GPU (NVIDIA) で加速する

デフォルトは CPU レンダリング (Mesa llvmpipe) で GPU 非依存ですが、NVIDIA GPU で RViz2 / Gazebo の描画を加速できます。

ホスト要件:
- NVIDIA ドライバがインストールされている Linux ホスト
- [nvidia-container-toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html) が設定済み

pull 済みイメージから起動する場合（`--gpus all` を追加）:

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host --gpus all \
  -e DISPLAY=$DISPLAY \
  -e NVIDIA_DRIVER_CAPABILITIES=all \
  -e QT_X11_NO_MITSHM=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy
```

compose から起動する場合（`docker-compose.gpu.yml` を override で重ねる）:

```sh
docker compose -f docker-compose.yml -f docker-compose.gpu.yml up demo
```

GPU を使わない通常起動は `docker-compose.yml` 単体、`--gpus all` 無しの `docker run` を使ってください（既存手順そのまま）。

## Intel / AMD 内蔵 GPU (DRI) で加速する

NVIDIA discrete GPU ではなく Intel / AMD の内蔵 GPU を使う場合は、container に host の DRM device (`/dev/dri`) を共有して hardware GL を使わせます。`docker-compose.dri.yml` を override で重ねてください:

```sh
xhost +local:docker

# お試し/デモ (demo service)
docker compose -f docker-compose.yml -f docker-compose.dri.yml up demo

# 開発用 (dev service。override は dev にも付くので bind mount 開発でも同様に重ねる)
docker compose -f docker-compose.yml -f docker-compose.dri.yml run --rm dev
```

これは `/dev/dri` の共有と `group_add: [video, render]` を dev / demo に足すだけの override です。これがないと、後述の「RViz / Gazebo の片方だけ GUI を出すと GL 初期化に失敗する」問題 (#229) が起きます。

前提として host 側に `/dev/dri/{card*, renderD*}` が存在すること (`ls -l /dev/dri` で確認)。**GPU device の無い host (SSH-only / CI 等) でこの override を重ねると `/dev/dri` バインド失敗で container が起動できない**ため、その環境では override を重ねず `docker-compose.yml` 単体 (software 経路) を使ってください。

> **container image 側の group**: 本 image のベース (`osrf/ros:<distro>-desktop-full`, Ubuntu 24.04) は `video` / `render` group を持つため `group_add` は名前解決できます。独自ベース image に差し替えてこれらの group が無いと compose が `group_add` でエラーになるので、その場合は numeric GID 指定 (下記) に切替えてください。

> **GID 不一致の注意**: hardware GL は render node (`/dev/dri/renderD128`, host の `render` group 所有) 経由で動きます。container 側 `render` / `video` group の GID が host と一致しないと、device は見えても permission denied で software (llvmpipe) に落ちます。その場合は `ls -l /dev/dri/renderD128` で host 側の GID を確認し、override の `group_add` に numeric GID (`group_add: ["<その GID>"]`) を足してください。それでも不調なら GPU 共有を諦め、次節の software 経路を使ってください。

> **HW GL が効いているかの確認**: override を重ねて起動したら、`docker compose ... exec dev glxinfo | grep "OpenGL renderer"` (要 `mesa-utils`) で renderer 名を見ます。Intel/AMD の GPU 名 (例 `Mesa Intel(R) ...`) が出れば hardware GL、`llvmpipe` なら software に落ちています (GID 不一致を疑う)。手早くは RViz 起動ログに `failed to load driver: iris` / `glx: failed to create dri3 screen` が出ていないかでも判別できます。

## RViz / Gazebo の片方だけ GUI を出すと GL 初期化に失敗する (Docker / Intel iGPU)

`docker-compose.dri.yml` を使わず、かつ `rviz` / `gazebo_gui` の **片方だけ** を `false` にすると、Docker + Intel iGPU 環境で GUI が起動できないことがあります (#229)。原因は RViz (OGRE classic) と Gazebo gz-sim (OGRE2) で GL フォールバックの挙動が異なるためです。

- **`gazebo_gui:=false` (RViz だけ GUI)**: container が `/dev/dri` にアクセスできないと RViz の Mesa iris driver ロードが失敗します。RViz は software (llvmpipe) へ自動フォールバックしないため crash します (gz-sim GUI は自動フォールバックを持つので default の両 GUI 起動では露見しません)。
- **`LIBGL_ALWAYS_SOFTWARE=1` を shell 全体に export**: RViz は software で起動しますが、gz-sim GUI まで llvmpipe に巻き込まれて起動が極端に遅延 / silently 失敗します。env を一律設定すると片方が壊れます。

解決経路は次の 3 つです。

| 状況 | 推奨 |
|---|---|
| host に使える GPU がある | `docker-compose.dri.yml` (Intel/AMD) または `docker-compose.gpu.yml` (NVIDIA) を重ねて hardware GL を共有 |
| GPU 共有できない / SSH-only / CI | `gazebo_gui:=false` で gz-sim を headless にした上で `LIBGL_ALWAYS_SOFTWARE=1` を設定 (gz-sim GUI が無いので現象 2 を踏まず、RViz だけ software で起動) |
| GUI が一切不要 | `gazebo_gui:=false rviz:=false` で fully headless 起動 (WebUI / nav は動く) |

software 経路の例 (gz-sim は server only、RViz だけ表示):

```sh
# 重要: LIBGL_ALWAYS_SOFTWARE=1 は必ず gazebo_gui:=false (gz-sim を headless) とセットで使う。
# gazebo_gui を default (true) のままにすると現象 2 (gz-sim GUI が出ない) を踏む。
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY -e LIBGL_ALWAYS_SOFTWARE=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy \
  bash -lc "source /opt/ros/\${ROS_DISTRO}/setup.bash && source /ros2_ws/install/setup.bash && \
    ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml gazebo_gui:=false"
```

> `LIBGL_ALWAYS_SOFTWARE=1` を gz-sim GUI と同居させると現象 2 (gz-sim GUI が出ない) を踏むため、software 経路では必ず `gazebo_gui:=false` (gz-sim を headless) と組合せてください。両 GUI を hardware で出したい場合は上の DRI / NVIDIA override を使います。

## Humble で compose から試す

`docker compose` の default は `jazzy` (primary distro) ですが、`ARG ROS_DISTRO` で Humble (Gazebo Classic) に切り替え可能です:

```sh
ROS_DISTRO=humble docker compose build
ROS_DISTRO=humble docker compose up demo
```

ローカル image は `mapoi:dev-${ROS_DISTRO}` / `mapoi:demo-${ROS_DISTRO}` の形 (例: `mapoi:dev-jazzy` / `mapoi:demo-humble`) で distro 別に保存されます。Humble と Jazzy を同時に保持・並行実行可能で、distro 切替時の rebuild も不要です。

pull 済みイメージを使う場合は `ghcr.io/shimz-robotics/mapoi:humble` を直接 `docker run` する方が手軽です（上の「ビルド済みイメージから試す」節を参照）。Humble は Nav2 lifecycle 立ち上げに 30〜60 秒かかるので起動直後に kill しないでください。

> **Gazebo Classic の状態**: `humble` image は Gazebo Classic を含みますが、**Gazebo Classic 自体は 2025-01 に EoL** (osrf による最終リリース 11 系で archived)、新規バグ修正や apt 配布が止まっています。新規プロジェクトでは Jazzy + gz-sim (Gazebo Harmonic、active maintained) を推奨します。ROS 2 Humble 自体の EoL は 2027-05 ですが、シミュレータ側の状況がより制約的です。
