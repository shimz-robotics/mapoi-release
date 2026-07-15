# mapoi_turtlebot3_example

> Japanese version: [README.ja.md](./README.ja.md)

A sample package for trying out mapoi in the TurtleBot3 simulation environment.
You can walk through the whole flow, from building a map with SLAM to autonomous navigation using Navigation2.

## Installing dependencies

Using rosdep:

```sh
# cd path/to/your_ws/
rosdep install --from-paths src --ignore-src -r -y
```

Installing manually:

```sh
sudo apt install -y \
  ros-${ROS_DISTRO}-turtlebot3-simulations \
  ros-${ROS_DISTRO}-turtlebot3-cartographer \
  ros-${ROS_DISTRO}-turtlebot3-navigation2 \
  ros-${ROS_DISTRO}-mouse-teleop
```

## Quick start

```sh
export TURTLEBOT3_MODEL=burger
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml
```

Test autonomous navigation to a POI from a separate terminal:

```sh
ros2 topic pub -1 /mapoi/nav/goal_pose_poi std_msgs/msg/String "{data: goal}"
```

### Per-feature demo routes (#230)

The `turtlebot3_world` map defines a set of tutorial routes that each exercise one of mapoi's main features, plus a `tour_full` route that hits every feature in one pass — 4 routes total. It uses an abstract 6-POI naming scheme (designed to be reusable on your own robot).

Recommended order:

1. **`tutorial_01_basic`** — a basic navigation health check (walks through `start → basic_waypoint → goal` in order)
2. Try the WebUI / RViz **map switch / POI editing** in the browser (no route needed)
3. **`tutorial_02_landmark`** — checks the landmark feature (registers `route.landmarks: [audio_landmark]` on the same waypoints as `tutorial_01`). If the demo subscriber is running, `audio_guide_node` mocks a voice announcement on `audio_landmark`'s `EVENT_ENTER`; if not, confirm the event fires via `ros2 topic echo /mapoi/events`
4. **`tutorial_03_pause`** — checks the pause feature (auto-pauses at `pause_waypoint`). If the demo subscriber is running, `camera_node` mocks a capture on the `capture_trigger` tag while paused and publishes resume; if not, manually run `ros2 topic pub /mapoi/nav/resume` from a separate shell
5. **`tour_full`** — the full combined demo

Example commands for each route:

```sh
# Basic: just confirm movement
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_01_basic}"

# Landmark: run `ros2 topic echo /mapoi/events` in another terminal and
# confirm audio_landmark's EVENT_ENTER fires (when the demo subscriber isn't running)
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_02_landmark}"

# Pause + manual resume (when the demo subscriber isn't running / default): resume from another shell after it stops
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_03_pause}"
ros2 topic pub -1 /mapoi/nav/resume std_msgs/msg/String "{data: ''}"

# Pause + auto-resume (opt-in): only if mapoi_nav2_bridge was started with a positive
# auto_resume_timeout_sec, the bridge auto-resumes after N seconds on the same tutorial_03_pause route
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_03_pause}"

# Full combined demo
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tour_full}"
```

> **NOTE**: the demo subscribers (`audio_guide_node` / `camera_node`) are **not** included in `turtlebot3_navigation.launch.yaml` (split out in #238). The `audio_guide` / `camera` logs only appear when `mapoi_event_samples.launch.yaml` is launched separately. The demo still works as intended even in a mode where you just watch `/mapoi/events` fire without bringing up a subscriber, using the commands above.

### Headless launch (no GUI)

For CI / SSH sessions / lightweight verification, you can bring things up without launching the RViz2 / Gazebo GUIs. The WebUI (http://localhost:8765) remains available as usual.

```sh
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml \
  rviz:=false gazebo_gui:=false
```

Individual control is also possible:

| arg | default | effect |
|---|---|---|
| `rviz` | `true` | Whether to launch RViz2 |
| `gazebo_gui` | `true` | Whether to launch the simulator GUI. Even with `false`, the server (Humble: `gzserver` / Jazzy: `gz sim -s`) still starts and physics simulation still runs |

`gazebo_gui` works on both Humble (Gazebo Classic) and Jazzy (gz-sim). `turtlebot3_navigation.launch.yaml` selects between distro-specific wrappers (`gazebo_headless_aware.launch.yaml` / `gz_sim_headless_aware.launch.yaml`) based on `ROS_DISTRO`. `rviz:=false` works regardless of distro.

## Building a map (SLAM)

### 1. Launch the simulator

```sh
export TURTLEBOT3_MODEL=burger
ros2 launch turtlebot3_gazebo turtlebot3_world.launch.py
```

### 2. Launch Cartographer

```sh
export TURTLEBOT3_MODEL=burger
ros2 launch turtlebot3_cartographer cartographer.launch.py use_sim_time:=True
```

### 3. Drive the robot

```sh
ros2 run mouse_teleop mouse_teleop --ros-args -r /mouse_vel:=/cmd_vel
```

Drive the robot with the mouse around the whole environment to complete the map.

### 4. Save the map

```sh
ros2 run nav2_map_server map_saver_cli -f maps/<map_name>/<map_name>
```

### 5. Edit the config file

Create a `mapoi_config.yaml` in the saved map directory and configure the POIs and routes.
See [the mapoi_server README](../mapoi_server/README.md) for format details.

## Sample client nodes

The following client nodes are included as usage examples for mapoi's services and actions.

| Node | Description |
| --- | --- |
| `get_maps_info_client` | Gets the list of maps |
| `get_pois_info_client` | Gets the list of POIs |
| `mapoi_switch_map_client` | Operator map switch via `/mapoi/nav/switch_map` |
| `navigate_to_pose_client` | Navigation to a specified position |
| `follow_waypoints_client` | Waypoint following |
| `print_initialpose` | Displays the initial pose |
| `audio_guide_node` | Plays a (mock) voice guide at an `audio_info` POI on receiving `mapoi/events`'s `EVENT_ENTER` (#88) |
| `camera_node` | Captures a (mock) photo at a `capture_trigger` POI and publishes `/mapoi/nav/resume` on receiving `mapoi/events`'s `EVENT_PAUSED` (#238) |

Example usage:

```sh
ros2 run mapoi_turtlebot3_example get_maps_info_client
ros2 run mapoi_turtlebot3_example get_pois_info_client
```

## PoiEvent-driven sample subscribers (#88 / #238)

A reference implementation that receives PoiEvent (`EVENT_ENTER` / `EVENT_PAUSED` / `EVENT_EXIT`) from `mapoi/events` and executes an action based on a custom tag. It serves as a template for scenarios like "play a voice guide" or "capture a photo + auto-resume" at specific POIs during route navigation.

This package includes the following nodes:
- `audio_guide_node` — `EVENT_ENTER` + `audio_info` tag → mock voice-guide playback (#88)
- `camera_node` — `EVENT_PAUSED` + `capture_trigger` tag → mock capture + `/mapoi/nav/resume` publish (#238)

A subscriber for `EVENT_EXIT` can be written the same way (just swap out the `event_type` comparison value), so add one if you need it. You can verify the pause/resume mechanism itself with the `tutorial_03_pause` route.

### How to launch (kept separate from the aggregate launch, #238)

`turtlebot3_navigation.launch.yaml` (the aggregate demo launch) does **not** include the demo subscribers. It only launches base navigation / Nav2 / mapoi_server / WebUI:

```sh
# Terminal 1: base navigation + Nav2 + mapoi_server + WebUI
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml
```

If you want to bring up the demo subscribers, launch `mapoi_event_samples.launch.yaml` in a separate shell:

```sh
# Terminal 2 (optional): starts audio_guide_node + camera_node
ros2 launch mapoi_turtlebot3_example mapoi_event_samples.launch.yaml
```

> The demo still works even without starting Terminal 2 (see "Verification scenarios" below for details).

### Verification scenarios

#### `tutorial_02_landmark` (checking audio_info / EVENT_ENTER)

```sh
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_02_landmark}"
```

When passing `audio_landmark`:
- **With the subscriber running (Terminal 2 up)**: `[AUDIO_GUIDE] play: poi='audio_landmark' ...` appears in the log
- **Without the subscriber**: running `ros2 topic echo /mapoi/events` in another shell lets you observe `event_type: 1 (EVENT_ENTER)`

The same behavior occurs on `tour_full` when passing `audio_landmark`, which is also listed in that route's `route.landmarks`.

#### `tutorial_03_pause` (checking capture_trigger / EVENT_PAUSED + auto resume)

```sh
ros2 topic pub -1 /mapoi/nav/route std_msgs/msg/String "{data: tutorial_03_pause}"
```

Arriving at `pause_waypoint` (tagged `waypoint` + `pause` + `capture_trigger`) → auto-pause → cmd_vel dwell:
- **With the subscriber running (Terminal 2 up)**: `[CAMERA] capture: poi='pause_waypoint' ...` → about 1.5s later, `[CAMERA] capture done; resume published` → the route resumes
- **Without the subscriber**: stays paused. Either resume manually from another shell, or start `mapoi_nav2_bridge` with a positive `auto_resume_timeout_sec` so the bridge auto-resumes:
  ```sh
  ros2 topic pub -1 /mapoi/nav/resume std_msgs/msg/String "{data: manual}"
  ```

### Custom tag naming guideline

The following naming convention is recommended for mapoi's **custom tags (user-defined tags other than system tags)**. This sample's `audio_info` is kept as-is to respect the existing yaml naming, but new tags should follow the patterns below:

| Naming pattern | Meaning | Example |
|---|---|---|
| `<action>_trigger` | The action triggered at this POI | `audio_trigger` (plays a voice guide), `capture_trigger` (triggers a capture) — this sample's `audio_info` would be equivalent to `audio_trigger` under a strict reading of the convention |
| `<action>_target` | A POI referenced as the target of an action | `capture_target` (the capture target, referenced via landmark) |
| `<state>` | A state category the POI represents | `hazard` / `atrium` / `inspection` |

### Implementation notes (subscriber side)

- `EVENT_ENTER` only fires **during route navigation, for route-registered POIs** (per the #220 spec; does not fire outside route navigation, i.e. in IDLE / GOAL mode)
- `EVENT_PAUSED` fires the moment **navigation stops (cmd_vel dwell) within a `pause`-tagged POI**, once per visit
- `EVENT_EXIT` resets each POI's per-visit state (so it can fire again on a re-visit)

See [the mapoi_interfaces README](../mapoi_interfaces/README.md#poieventmsg) and the header comment in [PoiEvent.msg](../mapoi_interfaces/msg/PoiEvent.msg) for the detailed spec.

## Sample maps

Two samples are included, together covering every combination of POI placement + tag pattern needed for the scenarios (#146).

| Map | Scenario | Key features it exercises |
| --- | --- | --- |
| `turtlebot3_world` | A feature-catalog demo (one route per feature + a combined `tour_full`) | System tags alone / combined, strict vs. loose `tolerance.yaw`, POIs with both `route.waypoints` + `landmarks`, `pause` with manual/auto resume, custom tags (`audio_info` / `capture_trigger`) working with the demo subscribers |
| `turtlebot3_dqn_stage1` | Obstacle-avoidance driving in a hazard sandbox (hazard / observation / pass-through points) | Combined tags (`landmark + custom`), a yaw-agnostic pass-through point via `tolerance.yaw=π` (a stand-in for pass_through), relaying through `pause`, use of `route.landmarks` |

### POI / tag composition

#### turtlebot3_world (routes: `tutorial_01_basic` / `tutorial_02_landmark` / `tutorial_03_pause` / `tour_full`)

| POI | tags | `tolerance` (xy / yaw) | Role |
| --- | --- | --- | --- |
| `start` | `waypoint` | 0.50 / 0.785 | First in the POI list = default initial pose (#144); starting point for all routes; matches the Gazebo spawn position (-2.0, -0.5) |
| `basic_waypoint` | `waypoint` | 0.35 / 0.785 | A southern relay point for `tutorial_01` / `02` / `tour_full` (a pure waypoint) |
| `pause_waypoint` | `waypoint`, `pause`, `capture_trigger` | 0.30 / 3.14 | The `pause` POI for `tutorial_03` / `tour_full`; when the demo subscriber is running, `camera_node` mocks a capture while paused |
| `goal` | `waypoint` | 0.40 / **0.10** | The end point of `tutorial_01` / `02` / `tour_full` (**strict yaw**) |
| `audio_landmark` | `landmark`, `audio_info` | 0.35 / 0.785 | Listed in `route.landmarks` for `tutorial_02` / `tour_full`; when the demo subscriber is running, `audio_guide_node` fires on `EVENT_ENTER` |

> **NOTE**: `tutorial_01_basic` / `tutorial_03_pause` omit `route.landmarks`, so they also double as an implicit demo that the yaml schema works correctly when `landmarks` is omitted.

> **NOTE**: consecutive `pause` firing (the role formerly played by the demo `corridor_a` / `corridor_b`) has been removed from these demo routes. Verifying consecutive-pause behavior is planned as a follow-up pinned by a launch_test (the current `test_poi_event_route_integration.py` only pins a single fire).

> **NOTE**: the contrast in how `EVENT_ENTER` behaves depending on whether it's listed in `route.landmarks` (only listed POIs publish) is deliberately left out of the demo config and instead pinned via an inline mock inside a launch_test (handled in #236). Keeping the demo config minimal and confining feature testing to the test side avoids confusing first-time users.

#### turtlebot3_dqn_stage1 (routes: `avoidance_a` / `avoidance_b`)

| POI | tags | Role |
| --- | --- | --- |
| `start_zone` | `waypoint` | First in the POI list = default initial pose (#144) |
| `checkpoint_west` | `waypoint`, `checkpoint` | A yaw-agnostic pass-through point (`tolerance.yaw=π`) |
| `checkpoint_east` | `waypoint`, `checkpoint` | A yaw-agnostic pass-through point (`tolerance.yaw=π`) |
| `pause_intersection` | `waypoint`, `pause` | Auto-stops at the intersection |
| `north_goal` | `waypoint` | The northern goal, strict with `tolerance.yaw=0.10` |
| `hazard_south` | `landmark`, `hazard` | A hazard with combined tags (not a valid Nav2 goal) |
| `observation_point` | `landmark`, `observation` | An observation target (a reference point during route navigation) |

> **NOTE**: `hazard` / `checkpoint` / `observation` are user-defined custom tags declared in this map's `custom_tags` (not system tags; the system tags are `waypoint` / `landmark` / `pause`).

> **NOTE**: `landmark × pause` is not combined here, since it's rejected by the #143 validation.

Reference: [TurtleBot3 E-Manual (Simulation)](https://emanual.robotis.com/docs/en/platform/turtlebot3/simulation/)
