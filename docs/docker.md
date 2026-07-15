# Try with Docker (demo)

> Japanese version: [docker.ja.md](./docker.ja.md)

The Docker images are for **demo / evaluation purposes**. To integrate mapoi into your own robot, see [Integrating into your own robot](./integration.md) (apt/rosdep distribution is planned for the future).

A Linux host is assumed; you can try the Turtlebot3 sample with Gazebo / RViz2 / Web UI.

## Prerequisites

- Docker Engine + Docker Compose v2 (when building from source)
- Linux host (GUI display via X11)
- No GPU required (runs with CPU rendering)

## Try from prebuilt images (fastest)

Just pull the images published on ghcr.io and run.

### Jazzy (gz-sim, recommended)

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY \
  -e QT_X11_NO_MITSHM=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy
```

### Humble (Gazebo Classic, Classic EoL 2025-01)

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY \
  -e QT_X11_NO_MITSHM=1 \
  -e GAZEBO_MODEL_DATABASE_URI= \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:humble
```

Nav2 lifecycle bringup takes about 30-60 seconds, so do not kill the container right after startup; wait a moment before accessing the Web UI.

Once started, open http://localhost:8765 in your browser to see the Web UI.

If the X11 connection is refused, try `xhost +local:` (allows all connections via the local unix socket). The user inside the distributed image is fixed to UID 1000, so `$HOME/.Xauthority` is not bind mounted; X11 access control is delegated to `xhost`.

> **Cleanup**: `xhost +local:docker` / `xhost +local:` grant broad access to the X server, so after the demo it is safer to revert with `xhost -local:docker` (revoke individually) or `xhost -` (revoke all).

Available tags:

| Tag | Description |
| --- | --- |
| `latest` | Recommended (same as `jazzy`, primary distro) |
| `jazzy` | ROS 2 Jazzy + gz-sim (Gazebo Harmonic), primary distro |
| `humble` | ROS 2 Humble + Gazebo Classic, maintained for Humble environments (Gazebo Classic reached EoL in 2025-01; ROS 2 Humble itself is supported until 2027-05. Jazzy + gz-sim recommended for new projects) |
| `vX.Y.Z` | Release-tagged version (primary distro: jazzy) |
| `vX.Y.Z-jazzy` | Jazzy build of the release tag |
| `vX.Y.Z-humble` | Humble build of the release tag |

You can check the ROS 2 version inside an image with `docker inspect ghcr.io/shimz-robotics/mapoi:<tag> | grep ROS_DISTRO`.

## Troubleshooting

### Web UI stuck at "Navigation unavailable"

The Navigation badge in the WebUI shows the readiness of the nav backend (Nav2 action server + mapoi). Check the following in order.

1. **Wait right after startup**: Nav2 lifecycle activation takes 30-60 seconds. The unavailable indication is normal during that time; once everything is up it changes to "Navigation connected".
2. **Refresh the image**: `jazzy` / `latest` are rolling tags tracking main, and `docker run` silently reuses your local cache. Run `docker pull ghcr.io/shimz-robotics/mapoi:jazzy` to make sure you are not holding an old previously-pulled image. For reproducible verification, consider using a release tag (`vX.Y.Z`).
3. **GUI (display) failures do not stop nav**: If `docker logs <container>` shows `could not connect to display` / `qt.qpa.xcb`, the container cannot connect to X and the RViz / Gazebo GUIs failed to start. Even without the GUIs, the nav backend and WebUI still work (this behavior requires an image from #295 or later; with an older cached image, a GUI crash stops the entire launch — update via `docker pull` in step 2). If you also want the RViz / Gazebo windows, run `xhost +local:docker` before starting (see "Try from prebuilt images" above for details).
4. **Use only the WebUI (no GUI needed)**: To run just the WebUI / nav without starting GUIs, start headless.

   ```sh
   docker run --rm -it --network host --ipc host \
     ghcr.io/shimz-robotics/mapoi:jazzy \
     bash -lc "source /opt/ros/\${ROS_DISTRO}/setup.bash && source /ros2_ws/install/setup.bash && \
       ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml gazebo_gui:=false rviz:=false"
   ```

## Build from source with docker compose (for developers)

To try local changes, clone the repository and build with compose.

> **`ROS_DISTRO` environment variable behavior**: The `docker compose` image / build switches distro via `${ROS_DISTRO:-jazzy}`.
> - If `ROS_DISTRO` from the shell / `.env` is **non-empty**, that value is used (e.g. `humble` if you have sourced ROS 2 Humble on the host, `jazzy` for Jazzy)
> - If **empty or unset**, the fallback default is `jazzy` (primary distro)
> - **To temporarily try another distro**, prefix the command like `ROS_DISTRO=humble docker compose ...` (does not affect the parent shell's `ROS_DISTRO`)
> - Local images are stored per distro in the form `mapoi:<service>-${ROS_DISTRO}` (e.g. `mapoi:dev-jazzy` / `mapoi:demo-humble`), so they can coexist
> - Details: [Docker Compose interpolation](https://docs.docker.com/reference/compose-file/interpolation/)

> **DDS environment handling**: DDS environment variables inside the container are set according to the following policy:
> - **`RMW_IMPLEMENTATION`**: **inherits** the host value via the shell's `${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}` (falls back to `rmw_fastrtps_cpp` if unset). If it does not match the host, container <-> host DDS discovery over `network_mode: host` + `ipc: host` will not work, so if the host uses cyclonedds, align the container to cyclonedds as well
> - **`ROS_DOMAIN_ID`**: likewise inherited from the host via `${ROS_DOMAIN_ID:-0}`, defaulting to 0 if unset. This respects setups that separate domains for multi-robot / parallel simulation
> - **`CYCLONEDDS_URI`**: always **fixed to empty** (the host's custom URI is not inherited). Even if the host exports `CYCLONEDDS_URI=/path/to/custom.xml` for another workspace, the container runs with the cyclonedds default config (this was the root cause of a past SwitchMap / discovery failure -> blocking)
> - To override (e.g. try another RMW, specify another domain):
>   - `ROS_DISTRO=humble docker compose run --rm --env ROS_DOMAIN_ID=42 dev` for an individual override
>   - To use the host's cyclonedds custom config inside the container as well (rare):
>     `ROS_DISTRO=humble docker compose run --rm --env CYCLONEDDS_URI=file:///path/to/cyclonedds.xml dev`
> - **Note**: `docker compose up` does not accept the `--env` flag, so if you need an override via `up`, edit `docker-compose.yml` or use a compose override file

```sh
git clone https://github.com/shimz-robotics/mapoi.git
cd mapoi
xhost +local:docker
docker compose up demo
```

Once started, open http://localhost:8765 in your browser to see the Web UI.

## Development (bind mount host sources, build inside the container)

```sh
# First build
docker compose build dev

# Enter a shell (the host's ./ is mounted at /ros2_ws/src/mapoi; --name gives the
# container a name so that other terminals can join later via docker exec)
docker compose run --rm --name mapoi-dev dev

# Inside the container
cd /ros2_ws
rosdep update --rosdistro ${ROS_DISTRO}
rosdep install --from-paths src --ignore-src -r -y
colcon build --symlink-install
source install/setup.bash
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml
```

You can edit sources with `git` / VSCode on the host and rebuild inside the container iteratively.

### Enter the same container from multiple terminals

If you want to run `ros2 topic echo` / `ros2 topic pub` from another terminal, re-running `docker compose run --rm dev` in that terminal starts a **new container** without `/ros2_ws/install/` (= the output of `colcon build` in the first one), so `ros2 topic echo` fails to resolve msg types (`mapoi_interfaces/msg/PoiEvent` etc. are unknown). Use `docker exec` to enter the same container:

```sh
# Enter the container started in the first terminal
docker exec -it mapoi-dev bash
# Inside the container (reuses the first one's install/ as is)
source /ros2_ws/install/setup.bash
ros2 topic echo /mapoi/events
```

DDS communication does work from another container over `network_mode: host`, but the install dir with the msg types is container-specific, so the observing side cannot echo without the same install dir.

#### When `Container mapoi-dev Error response from daemon: Conflict ... name "/mapoi-dev" is already in use` appears

If you exited the previous `docker compose run --rm --name mapoi-dev dev` with Ctrl-C or similar, the stopped container `mapoi-dev` may remain as an orphan. It collides with the same name on the next `run`, so remove it before restarting:

```sh
docker rm mapoi-dev    # remove the stopped container
ROS_DISTRO=jazzy docker compose run --rm --name mapoi-dev dev    # start again
```

The `--rm` flag removes the container on a normal exit (Ctrl-D / `exit`), but there are paths (Ctrl-C or forced kill) where it remains.

## Adjusting permissions (UID/GID)

If the `ros` user inside the container (default UID 1000) and the host user's UID/GID do not match, **write operations fail with permission denied** across the bind mount (`./:/ros2_ws/src/mapoi`). Typical cases:

- Clicking **Save** in the POI Editor Panel (writes to mapoi_config.yaml)
- install/build/log output of `colcon build` inside the container (may also remain on the host)
- Editing source files inside the container

The following steps are **required if the host UID differs from 1000** (hosts with UID 1000 work with the defaults):

```sh
# Recommended: generate .env, automatically injected into docker compose (.gitignore + .dockerignore already set)
echo "USER_ID=$(id -u)"   > .env
echo "GROUP_ID=$(id -g)" >> .env
docker compose build dev    # .env is read automatically

# Or prefix each command:
USER_ID=$(id -u) GROUP_ID=$(id -g) docker compose build dev
```

Once `.env` is created, the host UID is applied automatically to all compose commands (build / run / up). See [.env.example](../.env.example) for the meaning of each key and comments. The UID is baked in at build time for each per-distro image (mapoi:dev-${ROS_DISTRO}), so rebuild the target distro's image after changing the UID.

## Accelerating with a GPU (NVIDIA)

The default is CPU rendering (Mesa llvmpipe) with no GPU dependency, but you can accelerate RViz2 / Gazebo rendering with an NVIDIA GPU.

Host requirements:
- Linux host with the NVIDIA driver installed
- [nvidia-container-toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html) configured

To start from a pulled image (add `--gpus all`):

```sh
xhost +local:docker
docker run --rm -it --network host --ipc host --gpus all \
  -e DISPLAY=$DISPLAY \
  -e NVIDIA_DRIVER_CAPABILITIES=all \
  -e QT_X11_NO_MITSHM=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy
```

To start from compose (layer `docker-compose.gpu.yml` as an override):

```sh
docker compose -f docker-compose.yml -f docker-compose.gpu.yml up demo
```

For normal startup without a GPU, use `docker-compose.yml` alone and `docker run` without `--gpus all` (existing steps as-is).

## Accelerating with an Intel / AMD integrated GPU (DRI)

If you use an Intel / AMD integrated GPU rather than an NVIDIA discrete GPU, share the host's DRM device (`/dev/dri`) with the container to use hardware GL. Layer `docker-compose.dri.yml` as an override:

```sh
xhost +local:docker

# Trial/demo (demo service)
docker compose -f docker-compose.yml -f docker-compose.dri.yml up demo

# Development (dev service; the override also applies to dev, so layer it the same way for bind mount development)
docker compose -f docker-compose.yml -f docker-compose.dri.yml run --rm dev
```

This override only adds sharing of `/dev/dri` and `group_add: [video, render]` to dev / demo. Without it, the "GL initialization fails when only one of RViz / Gazebo shows a GUI" issue described below (#229) occurs.

As a prerequisite, `/dev/dri/{card*, renderD*}` must exist on the host (check with `ls -l /dev/dri`). **On hosts without a GPU device (SSH-only / CI etc.), layering this override makes the container fail to start due to the `/dev/dri` bind failure**, so in those environments use `docker-compose.yml` alone (software path) without the override.

> **Groups in the container image**: The base of this image (`osrf/ros:<distro>-desktop-full`, Ubuntu 24.04) has the `video` / `render` groups, so `group_add` resolves the names. If you swap in a custom base image without these groups, compose errors on `group_add`; in that case switch to numeric GID specification (below).

> **GID mismatch caveat**: Hardware GL works via the render node (`/dev/dri/renderD128`, owned by the host's `render` group). If the GID of the container-side `render` / `video` groups does not match the host, the device is visible but permission denied drops you to software (llvmpipe). In that case check the host-side GID with `ls -l /dev/dri/renderD128` and add the numeric GID to the override's `group_add` (`group_add: ["<that GID>"]`). If it still misbehaves, give up on GPU sharing and use the software path in the next section.

> **Checking whether HW GL is in effect**: After starting with the override, look at the renderer name with `docker compose ... exec dev glxinfo | grep "OpenGL renderer"` (requires `mesa-utils`). If an Intel/AMD GPU name appears (e.g. `Mesa Intel(R) ...`), it is hardware GL; if `llvmpipe`, it fell back to software (suspect a GID mismatch). As a quick check, you can also look for `failed to load driver: iris` / `glx: failed to create dri3 screen` in the RViz startup log.

## GL initialization fails when only one of RViz / Gazebo shows a GUI (Docker / Intel iGPU)

Without `docker-compose.dri.yml`, setting **only one** of `rviz` / `gazebo_gui` to `false` can prevent the GUI from starting in a Docker + Intel iGPU environment (#229). The cause is that RViz (OGRE classic) and Gazebo gz-sim (OGRE2) differ in GL fallback behavior.

- **`gazebo_gui:=false` (RViz GUI only)**: If the container cannot access `/dev/dri`, RViz's Mesa iris driver load fails. RViz does not automatically fall back to software (llvmpipe), so it crashes (the gz-sim GUI has automatic fallback, so this does not surface with the default both-GUI startup).
- **Exporting `LIBGL_ALWAYS_SOFTWARE=1` to the whole shell**: RViz starts in software, but the gz-sim GUI also gets dragged into llvmpipe and its startup is extremely delayed / fails silently. Setting the env uniformly breaks one of the two.

There are three resolution paths.

| Situation | Recommendation |
|---|---|
| Host has a usable GPU | Layer `docker-compose.dri.yml` (Intel/AMD) or `docker-compose.gpu.yml` (NVIDIA) to share hardware GL |
| GPU sharing not possible / SSH-only / CI | Make gz-sim headless with `gazebo_gui:=false`, then set `LIBGL_ALWAYS_SOFTWARE=1` (no gz-sim GUI means symptom 2 is avoided; only RViz starts in software) |
| No GUI needed at all | Fully headless startup with `gazebo_gui:=false rviz:=false` (WebUI / nav still work) |

Example of the software path (gz-sim server only, RViz display only):

```sh
# Important: always use LIBGL_ALWAYS_SOFTWARE=1 together with gazebo_gui:=false (gz-sim headless).
# Leaving gazebo_gui at the default (true) triggers symptom 2 (gz-sim GUI does not appear).
docker run --rm -it --network host --ipc host \
  -e DISPLAY=$DISPLAY -e LIBGL_ALWAYS_SOFTWARE=1 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  ghcr.io/shimz-robotics/mapoi:jazzy \
  bash -lc "source /opt/ros/\${ROS_DISTRO}/setup.bash && source /ros2_ws/install/setup.bash && \
    ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml gazebo_gui:=false"
```

> Combining `LIBGL_ALWAYS_SOFTWARE=1` with the gz-sim GUI triggers symptom 2 (gz-sim GUI does not appear), so on the software path always combine it with `gazebo_gui:=false` (gz-sim headless). To show both GUIs in hardware, use the DRI / NVIDIA overrides above.

## Try Humble via compose

The `docker compose` default is `jazzy` (primary distro), but you can switch to Humble (Gazebo Classic) via `ARG ROS_DISTRO`:

```sh
ROS_DISTRO=humble docker compose build
ROS_DISTRO=humble docker compose up demo
```

Local images are stored per distro in the form `mapoi:dev-${ROS_DISTRO}` / `mapoi:demo-${ROS_DISTRO}` (e.g. `mapoi:dev-jazzy` / `mapoi:demo-humble`). Humble and Jazzy can be kept and run in parallel at the same time, with no rebuild needed when switching distros.

If you use pulled images, it is easier to `docker run` `ghcr.io/shimz-robotics/mapoi:humble` directly (see "Try from prebuilt images" above). Humble takes 30-60 seconds for Nav2 lifecycle bringup, so do not kill it right after startup.

> **Status of Gazebo Classic**: The `humble` image includes Gazebo Classic, but **Gazebo Classic itself reached EoL in 2025-01** (archived at the final osrf release, the 11 series); new bug fixes and apt distribution have stopped. Jazzy + gz-sim (Gazebo Harmonic, actively maintained) is recommended for new projects. ROS 2 Humble itself reaches EoL in 2027-05, but the simulator side is the more constraining factor.
