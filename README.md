# mapoi_server

> Japanese version: [README.ja.md](./README.ja.md)

The main package of mapoi.
It manages map and POI information and serves it to other packages, and performs POI-name-based autonomous navigation and POI radius event detection.

## Integrating with your own robot

We provide a bringup launch file that starts the 4 core nodes (`mapoi_server` / `mapoi_nav2_bridge` / `mapoi_amcl_localization_bridge` / `mapoi_rviz2_publisher`) together, along with a simulator-integration bridge if needed. Include it from your own launch file like this:

```yaml
- include:
    file: "$(find-pkg-share mapoi_server)/launch/mapoi_bringup.launch.yaml"
    arg:
      - {name: maps_path, value: "/path/to/your/maps"}    # REQUIRED
      - {name: map_name, value: "initial_map_name"}        # REQUIRED
      - {name: config_file, value: "mapoi_config.yaml"}    # optional
      - {name: state_path, value: "/var/lib/mapoi"}        # optional (recommended for real-robot deployments, #297)
      - {name: simulator, value: "none"}                   # gazebo|gz|none (default: none)
      - {name: robot_entity_name, value: "burger"}         # required when simulator=gazebo|gz
      - {name: robot_sdf_path, value: "/path/.../model.sdf"} # required when simulator=gazebo
      - {name: init_world_name, value: "default"}          # required when simulator=gz (world name used when starting gz_sim)
```

`maps_path` and `map_name` are **required**. If `maps_path` is unset, points to a nonexistent path, or is not a directory, startup fails with an `RCLCPP_FATAL` log and exit code 1 (#163). To reuse the `mapoi_turtlebot3_example` samples, pass `$(find-pkg-share mapoi_turtlebot3_example)/maps`.

### Launching directly from the CLI

A minimal example of calling `ros2 launch` directly, without an include:

```bash
# Reuse the mapoi_turtlebot3_example sample maps (also works after apt / distributed install)
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=$(ros2 pkg prefix --share mapoi_turtlebot3_example)/maps \
  map_name:=turtlebot3_world

# Directly from a source tree (after sourcing inside ros2_ws)
# Note: assumes this repo is cloned to `<ws>/src/mapoi/` (`git clone .../mapoi.git src/mapoi` at the ws root).
# If you've laid packages out flat, e.g. `<ws>/src/mapoi_turtlebot3_example/`,
# adjust the path accordingly, or use the `pkg prefix --share` form above for reliability.
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=$(pwd)/src/mapoi/mapoi_turtlebot3_example/maps \
  map_name:=turtlebot3_dqn_stage1
```

#### Watch out for mixing up arguments

The semantics of each launch arg are as follows (a 3-level split accessed as `{maps_path}/{map_name}/{config_file}`):

| arg | What to pass | Example |
|---|---|---|
| `maps_path` | **the parent directory that bundles the map collection** (each map is a subdirectory under it) | `/path/to/maps` |
| `map_name` | the name of the map subdirectory directly under `maps_path` | `turtlebot3_world` |
| `config_file` | the config filename inside the map subdirectory (default: `mapoi_config.yaml`) | `mapoi_config.yaml` |

The path actually loaded is `{maps_path}/{map_name}/{config_file}`, which for the example above resolves to `/path/to/maps/turtlebot3_world/mapoi_config.yaml`.

Validation of the required `maps_path` / `map_name` (see above) happens in **two stages**: if `map_name` itself is unset, `ros2 launch` fails with `Required launch argument 'map_name' was not provided` (before the node starts, on the launch-system side); semantic violations such as `maps_path` not being a directory fail with `RCLCPP_FATAL` when the `mapoi_server` node starts.

**A common mistake**: passing a **config.yaml file path directly** as `maps_path`:

```bash
# WRONG: maps_path must be a directory, not a file
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=./maps/turtlebot3_dqn_stage1/mapoi_config.yaml \
  map_name:=turtlebot3_dqn_stage1
```

→ On startup, the `mapoi_server` node prints the following in a single line and exits FATAL — actual log text (partly Japanese; the Japanese sentence asks you to specify a valid maps directory path), quoted verbatim from the `RCLCPP_FATAL` in `mapoi_server.cpp`:

```
[FATAL] maps_path '...mapoi_config.yaml' does not exist or is not a directory. 正しい maps ディレクトリ path を指定してください (例: $(find-pkg-share mapoi_turtlebot3_example)/maps)。
```

`maps_path` is **the parent directory that bundles the map collection**, not a specific map directory or file. See the [Directory structure](#directory-structure) section at the end of this README for the directory layout.

The `simulator` arg controls whether a simulator-integration bridge is started:
- `gazebo` (Gazebo Classic / Humble): starts `mapoi_gazebo_bridge`. On an operator map switch, it swaps the `world_model` entity in Gazebo and respawns the robot (delete + spawn) at the coordinates of the **first POI in the list** (#144 dropped the old `initial_pose` system tag in favor of expressing this via yaml ordering)
- `gz` (gz-sim / Jazzy): starts `mapoi_gz_bridge` + `parameter_bridge` (`ros_gz_bridge`). On an operator map switch, it swaps the `world_model` entity in gz-sim and teleports the robot to the coordinates of the **first POI in the list** via `SetEntityPose` (unlike Classic, gz-sim has a set_pose service, so delete + spawn isn't needed)
- `none` (default): no bridge is started (real-robot deployment)

If you also want the Web UI, include `mapoi_webui.launch.yaml` as well:

```yaml
- include:
    file: "$(find-pkg-share mapoi_webui)/launch/mapoi_webui.launch.yaml"
    arg:
      - {name: maps_path, value: "/path/to/your/maps"}
      - {name: map_name, value: "initial_map_name"}
```

> **NOTE**: `mapoi_webui` retrieves system tags via `mapoi_server`'s `mapoi/get_tag_definitions` service. Since the system-tags display goes through this service call, `mapoi_webui` doesn't need `mapoi_server` installed as a package even when launched standalone (though a `mapoi_server` node must be running to respond as the service server).

See the `mapoi_turtlebot3_example` package for a working example with TurtleBot3.

## Nodes

### mapoi_server

A node that loads the map and POI config files and exposes information-retrieval services.

#### Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `maps_path` | `string` | **REQUIRED** | Path to the maps directory (made required after sample maps were dropped in #163) |
| `map_name` | `string` | `turtlebot3_world` | Name of the map to load at startup |
| `config_file` | `string` | `mapoi_config.yaml` | Name of the config file |
| `state_path` | `string` | `""` (disabled) | A **writable** directory used to persist the last-selected map (#297). Empty disables persistence (legacy behavior). See the "Persisting map selection and restoring on restart" section below for details |

#### Persisting map selection and restoring on restart (`state_path`, #297)

Since a `mapoi_server` restart (crash / supervisor restart) creates a brand-new DDS publisher, the startup initial-pose publish (#144) reaches not only late-joining subscribers but also **any localization bridge that's already running, as a fresh sample** (an active push). With `state_path` unset (the default), even if the operator had already switched to a different map, a restart re-publishes the first POI of the startup parameter `map_name`, **teleporting a running robot's estimated pose to the startup map's first POI** (the same happens even on a same-map restart with no map switch). On top of that, since the map context itself reverts to the startup parameter, goal / route POI-name resolution also falls back to the old map.

If `state_path` points to a writable directory, the current map name is recorded to `<state_path>/last_selected_map` (on startup and on a successful `mapoi/select_map`, via an atomic tmp + rename write), and behavior branches based on whether this state file exists:

- **No state file (a genuine first boot)**: publishes the first POI as before (preserves #144's automatic initial pose)
- **State file present (a restart during operation)**: restores the map context to the last-selected map, and publishes the initial pose as **clear only** (`poi_name` empty, ignored by subscribers). Does not affect the robot's currently running position estimate

Operational notes:

- Restoring on restart takes **priority** over the startup parameter `map_name`. If you explicitly want to bring the system up on a different map, either call `mapoi/select_map` or delete the state file before starting (deleting it reverts the next boot to first-boot behavior)
- Even if `maps_path` is shared across multiple robots/configurations, `state_path` should point to a **local directory per robot** (e.g. `/var/lib/mapoi`). `mapoi_server` does not expand `~` in parameter values, so either give an **absolute path**, or pass it through a route where shell expansion applies (e.g. `state_path:=$HOME/.ros/mapoi`)
- If the state file's map doesn't exist under `maps_path` (map deleted / path changed), a WARN is emitted and the startup parameter's map is used instead (in this case too, only a clear is published)
- If `state_path` is not writable, startup fails fast with `RCLCPP_FATAL` and exit code 1 (the same policy as the `maps_path` validation in #163). A write failure during operation (e.g. disk full) only emits a WARN and the node keeps running

#### Services

| Service | Type | Description |
| --- | --- | --- |
| `mapoi/get_maps_info` | `GetMapsInfo` | Gets the list of available maps and the current map name |
| `mapoi/get_pois_info` | `GetPoisInfo` | Gets all POIs on the current map |
| `mapoi/get_route_pois` | `GetRoutePois` | Gets the POIs on a route. If no route matches `route_name`, returns `success=false` + `error_message` (#342) |
| `mapoi/get_routes_info` | `GetRoutesInfo` | Gets the list of available routes |
| `mapoi/get_tag_definitions` | `GetTagDefinitions` | Gets the tag definitions (system/user) |
| `mapoi/select_map` | `SelectMap` | Switches the current map context (does not call Nav2). `map_name` only accepts a single path segment directly under `maps_path`; separators, `.`, and `..` are rejected (#328) |
| `mapoi/reload_map_info` | `std_srvs/Trigger` | Reloads the config file |
| `mapoi/request_initial_pose` | `RequestInitialPose` | The sole path for receiving `{map_name, poi_name}` and publishing `mapoi/initialpose_poi` (#211). An empty `poi_name` clears (ignored by subscribers). If a non-empty `map_name` doesn't match the current map, nothing is published and `success=false` (#299) |

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/config_path` | `std_msgs/String` | Path of the current config file (published periodically, transient_local QoS) |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest` | Notification of an initial-pose candidate (transient_local QoS, depth=1). The sole publisher that fires only via the `mapoi/request_initial_pose` service (#211) |

### mapoi_nav2_bridge

A node that performs POI-name-based autonomous navigation and POI radius event detection.

The `MapoiNav2Bridge` class implementation is split across five translation units for
maintainability (#345, a two-step split now complete): `src/mapoi_nav2_bridge.cpp` (node
setup, pause/resume/cancel dispatch, and the POI radius/pause judgment engine — the
shared core), `src/mapoi_nav2_bridge_route.cpp` (route navigation: `FollowWaypoints` and
the mapoi-driven waypoint-arrival mode), `src/mapoi_nav2_bridge_goal.cpp` (single-goal
navigation: `NavigateToPose`), `src/mapoi_nav2_bridge_map_switch.cpp` (map switching:
`switch_map` subscription, `select_map` sync, Nav2 `LoadMap`, and the initial-pose
request), and `src/mapoi_nav2_bridge_backend_status.cpp` (the 1Hz backend-status
publisher, which runs on its own callback group — see the file's header comment before
touching it). All five compile into the single `mapoi_nav2_bridge` executable; the split
is internal-only and does not change the node's external behavior (topics, services,
parameters, or logs).

#### Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `radius_check_hz` | `double` | `5.0` | Frequency (Hz) of the POI radius check |
| `hysteresis_exit_multiplier` | `double` | `1.15` | Threshold multiplier for the EXIT determination (prevents chattering) |
| `map_frame` | `string` | `map` | Parent TF frame |
| `base_frame` | `string` | `base_link` | Child TF frame |
| `auto_resume_timeout_sec` | `double` | `0.0` | Auto-resume timeout (seconds) after an `EVENT_PAUSED` fires (#231). `0.0` disables it (current behavior, waits indefinitely for an external `/mapoi/nav/resume`). A positive value enables an opt-in behavior for demo / autonomous-driving scenarios of "call an internal resume N seconds after PAUSED." Negative values are clamped to `0.0` at startup. No dynamic reconfigure support |
| `cmd_vel_topic` | `string` | `cmd_vel` | Topic name to subscribe to for `cmd_vel`, used for `EVENT_PAUSED` detection. Assumes the same topic Nav2 uses (the stop-detection source) |
| `cmd_vel_msg_type` | `string` | `auto` | Message type of `cmd_vel` (#249 / #251). `twist` / `twist_stamped` / `auto` (default = auto-selected from `ROS_DISTRO`). See the subsection below for details and cases requiring an override |
| `waypoint_arrival_mode` | `string` | `nav2` | Which side drives waypoint-arrival determination for routes (#243). In `mapoi` mode, even a single-shot Go determines arrival via the POI's own `tolerance.xy`+`tolerance.yaw` (#261). `nav2` / `mapoi`. See the "Waypoint arrival mode" section below for details. No dynamic reconfigure support (resolved at startup; unknown values fall back to `nav2`) |

##### `cmd_vel_msg_type` values and configurations requiring an override (#249 / #251)

The `cmd_vel` publisher type varies by ROS 2 distro / controller in use, split between `geometry_msgs/Twist` and `geometry_msgs/TwistStamped`:

- `twist` — subscribes as `geometry_msgs/Twist` (Humble Nav2 compatible)
- `twist_stamped` — subscribes as `geometry_msgs/TwistStamped` (Jazzy-and-later Nav2: `collision_monitor` / `docking_server` etc. have already moved to TwistStamped)
- `auto` (default) — auto-selected from the `ROS_DISTRO` environment variable (`humble` → `twist`, otherwise / unset → `twist_stamped`)

**A type mismatch doesn't just leave the subscription empty — it crashes the process**: creating two subscribers of different types on the same topic makes rcl crash with `invalid allocator` (reproduced on real hardware in #249). It's essential that the type `auto` selects matches the actual publisher type in use.

You need to override `auto` in the following mixed configurations:

| Case | Override |
| --- | --- |
| A custom controller on jazzy still publishes Twist | explicitly set `cmd_vel_msg_type: "twist"` |
| A custom controller on humble has already adopted TwistStamped | explicitly set `cmd_vel_msg_type: "twist_stamped"` |
| `ROS_DISTRO` is unset in a minimal CI / custom container, and you're using humble | explicitly set `cmd_vel_msg_type: "twist"` (the default fallback is `twist_stamped`) |

An unknown value (typo) emits a WARN and falls back to the safe side based on `ROS_DISTRO`.

##### Waypoint arrival mode (`waypoint_arrival_mode`, #243)

Switches **which side drives** the "this waypoint has been reached, advance to the next one" decision during route navigation.

| Value | Arrival determination driven by | Relationship between tolerance.xy and xy_goal_tolerance |
| --- | --- | --- |
| `nav2` (default) | Nav2's `FollowWaypoints` advances waypoints using `xy_goal_tolerance`. mapoi acts only as a radius observer (just emits `EVENT_ENTER` etc.) | `tolerance.xy` must be set **larger than** `xy_goal_tolerance` (otherwise, if Nav2's goal check stops the robot before it reaches the radius, ENTER never fires) |
| `mapoi` | mapoi sends `NavigateToPose` one waypoint at a time and advances on **arrival = OR((`tolerance.xy` ∧ `tolerance.yaw`) ∨ Nav2 SUCCEEDED)** (#265) | `tolerance.xy < xy_goal_tolerance` is possible (POIs can be made smaller) |

**Design points of `mapoi` mode:**

- **Unified arrival (#265)**: route mid-waypoints, the final goal, and single-shot Go (`mapoi/nav/goal_pose_poi`) are all judged by **OR((`tolerance.xy` ∧ `tolerance.yaw`) ∨ Nav2 SUCCEEDED)**. If the robot naturally settles within the radius and within `tolerance.yaw` of the target orientation, it advances/arrives immediately without waiting for Nav2 to finish (snappy); if it's off, it waits for Nav2 to finish aligning the pose (SUCCEEDED). This also catches the case where the robot turns in place after entering the radius and the yaw lines up later, since it's re-evaluated every tick. In `nav2` mode, both routes and Go are left entirely to Nav2 as before.
- **Per-POI `tolerance.yaw` is the knob for "does yaw matter here"**: the angular difference used for arrival determination is in `[0, π]` (max π ≈ 180°). For a pure pass-through point, set `tolerance.yaw` to π or above (e.g. `3.1416`, matching the demo config's `basic_waypoint`) for complete fly-through regardless of yaw. Just under π (`3.14`) is practically equivalent, but leaves a very narrow band around directly-facing-backward (±0.09°) where the radius alone won't register arrival (Nav2 SUCCEEDED is the safety net). For points where orientation matters (e.g. photo capture, the final goal), make it small so orientation is enforced. To make `tolerance.yaw` actually take effect (just like xy), also lower Nav2's `yaw_goal_tolerance` to at or below `tolerance.yaw` (since the OR's effective tolerance is the looser of the two). **Note**: a POI where yaw doesn't matter (`tolerance.yaw >= π`) is drawn in RViz/WebUI as a filled circle overlay instead of a sector (#267).
- **OR arrival (prevents getting stuck)**: even if `tolerance.xy`/`tolerance.yaw` are set smaller than Nav2's goal_checker, using Nav2 SUCCEEDED as a fallback keeps the route from stalling (an AND-only check could deadlock).
- **To truly shrink the effective arrival radius, Nav2 must be tightened too**: the OR's effective arrival radius is "whichever is looser." Even with `tolerance.xy=0.15`, if Nav2 stops at `xy_goal_tolerance=0.25`, the effective radius stays 0.25 (and ENTER never fires). To actually shrink a POI, also lower `goal_checker.xy_goal_tolerance` in `param/<distro>/burger.yaml` to at or below `tolerance.xy`.
- **`landmark` is out of scope**: a `landmark` is an ENTER-trigger radius (for voice guidance, etc.), not a waypoint, so it doesn't participate in this mode's progression decision; its radius can stay as wide as needed to cover the route.
- **Pause waypoints**: auto-pauses on unified arrival (`tolerance.xy` ∧ `tolerance.yaw`), and advances to the next waypoint via `mapoi/nav/resume`. Make `tolerance.yaw` small if orientation matters for the capture, or large if it doesn't.

**Trying it out with the demo (turtlebot3_example):**

```sh
# Start in mapoi mode (for experiments that shrink POIs, also lower burger.yaml's goal_checker.xy_goal_tolerance)
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml waypoint_arrival_mode:=mapoi
```

The demo defaults to `mapoi` mode (#263). The sample config (`turtlebot3_world/mapoi_config.yaml`) already sets `tolerance.yaw` per POI (loose for pass-through points, `0.1` and strict for the final `goal`). When shrinking POIs in `mapoi` mode, adjust the yaml's `tolerance` together with the Nav2 params while checking in sim.

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/nav/switch_map` | `std_msgs/String` | Operator map-switch instruction. On receipt, executes `mapoi/select_map` → Nav2 `LoadMap` → `mapoi_server` publishing `mapoi/initialpose_poi` via the `mapoi/request_initial_pose` service (= triggers an initial-pose push to the localization bridge) |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | Navigates autonomously to the position of the given POI name |
| `mapoi/nav/route` | `std_msgs/String` | Navigates through the waypoints of the given route name in order |
| `mapoi/nav/pause` | `std_msgs/String` | Pauses navigation |
| `mapoi/nav/resume` | `std_msgs/String` | Resumes navigation |
| `mapoi/nav/cancel` | `std_msgs/String` | Cancels navigation |
| `mapoi/config_path` | `std_msgs/String` | Detects map switches (transient_local QoS) |

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `goal_pose` | `geometry_msgs/PoseStamped` | Publishes the goal position |
| `mapoi/nav/status` | `std_msgs/String` | Publishes navigation status in `"status"` or `"status:target"` form (e.g. `"navigating:kitchen"`, `"succeeded:patrol_route"`, `"paused:patrol_route"`, `"map_switching:turtlebot3_world"`, `"rejected:kitchen"`). `status` is one of `navigating` / `succeeded` / `aborted` / `canceled` / `paused` / `map_switching` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected`. `backend_unavailable` means a goal / route / resume could not be executed because a Nav2 action/service was unavailable (#198). `rejected` means a goal / route command was judged invalid and never executed **before being accepted** (a nonexistent POI name, a `landmark`-tagged POI given as a goal, the `mapoi/get_pois_info` / `mapoi/get_route_pois` services not ready, an empty route waypoint list, etc.) (#339, #355). `target` is a POI name (goal mode), a route name (route mode), or a map name; subscribers split on the first `:` to reconstruct it (so a `:` inside the target itself is preserved as part of the remainder). This is a **snapshot of the current state** via `transient_local` QoS (depth=1) — a late-joining subscriber receives the last state, but the history of state transitions cannot be reconstructed. Whether navigation is currently in progress is determined by `navigating` / `paused` / `map_switching`, and terminal states (`succeeded` / `aborted` / `canceled` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected`) are treated as the most recent result |
| `mapoi/nav/command_rejected` | `std_msgs/String` | An **event notification** for a rejected command (#354). The payload is just the target string of the rejected command. Since `mapoi/nav/status` is a latched state snapshot, a reject while navigating (`nav_mode_ != IDLE`) cannot write "rejected" there (#339, see above). This topic is an independent event axis: at the top of `publish_rejected_status`, it is **always published on every reject, regardless of `nav_mode_`** (so an operator can notice things like a typo'd goal sent mid-navigation). QoS is volatile (not `transient_local`, depth=10) — since an event is only meaningful "now," it is not replayed to late-joining subscribers. WebUI subscribes to this to show a transient toast via SSE (see `mapoi_webui/README.md`) |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | Publishes the navigation bridge's readiness summary at 1Hz (#198). `transient_local` QoS. A minimal 3-field message (`backend_type` / `backend_ready` / `reason`). The WebUI / panel use `backend_ready` to gate the navigation-operation UI as a whole. See [docs/backend-status.md](../docs/backend-status.md) for details |
| `mapoi/events` | `mapoi_interfaces/PoiEvent` | POI entry (`EVENT_ENTER`) during route navigation / navigation stop at a pause POI (`EVENT_PAUSED`) / exit (`EVENT_EXIT`) events (#220) |

#### Action clients

| Action | Type | Description |
| --- | --- | --- |
| `follow_waypoints` | `nav2_msgs/FollowWaypoints` | Waypoint following |
| `navigate_to_pose` | `nav2_msgs/NavigateToPose` | Single-goal navigation |

#### Service clients

| Service | Type | Description |
| --- | --- | --- |
| `request_initial_pose` | `RequestInitialPose` | Asks `mapoi_server` to publish the initial pose after Nav2 `LoadMap` completes (#211). `mapoi_nav2_bridge` does not publish `mapoi/initialpose_poi` directly |

#### POI radius event detection (`PoiEvent`)

Publishes 3 kinds of events on the `mapoi/events` topic (#220 simplified this from 4 types to 3). Detection is limited to **during route navigation (`nav_mode == ROUTE`)** + **route-registered POIs**; events do not fire outside route navigation (in `IDLE` / `GOAL` mode). This does not depend on `waypoint_arrival_mode`: events fire both on the `FollowWaypoints` path (`nav2` mode) and on the per-waypoint `NavigateToPose` path (`mapoi` mode, the demo default).

| event_type | Fires when |
| --- | --- |
| `EVENT_ENTER` | Entering a route POI's `tolerance.xy` radius |
| `EVENT_PAUSED` | **Navigation stops** within a `pause`-tagged POI's `tolerance.xy` (detected via cmd_vel dwell), only once per visit |
| `EVENT_EXIT` | Exiting a route POI beyond `tolerance.xy * hysteresis_exit_multiplier` |

Prerequisites for detection:

- Gets the robot's position via a TF lookup (`map` -> `base_link`) (default 5Hz)
- For `pause`-tagged POIs, navigation is automatically paused on entry (and `EVENT_PAUSED` is published after nav stops)
- `EVENT_PAUSED` assumes the controller in use **keeps publishing cmd_vel = 0 while navigation is stopped** (the Nav2 default behavior). If the controller stops publishing cmd_vel while stationary, `EVENT_PAUSED` will not fire
- On a map switch, internal state is reset and monitoring resumes with the new POI list
- There is no `RESUMED`-equivalent event (resume can be observed via a client-side request and `mapoi/nav/status`)
- Setting `auto_resume_timeout_sec > 0.0` enables an opt-in behavior that internally calls resume N seconds after `EVENT_PAUSED` fires (#231). If an external `/mapoi/nav/resume` arrives before the timer, the pending timer is canceled; a route cancel, a new route command, or another `PAUSED` firing also cancels it via overwrite. The default `0.0` (= disabled) preserves the current behavior (waiting indefinitely)

### mapoi_amcl_localization_bridge

A localization bridge node for AMCL-compatible localization (a setup that receives `/initialpose` as `geometry_msgs/PoseWithCovarianceStamped`) (#209). By separating the AMCL adapter out of `mapoi_nav2_bridge`, the Navigation backend and Localization backend can be treated as independent specs. If you switch to slam_toolbox / NDT / a custom localization, provide your own bridge that satisfies the same topic spec as a replacement for this bridge (see the "Localization backend" section of [docs/backend-status.md](../docs/backend-status.md)).

#### Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `initial_pose_topic` | `string` | `/initialpose` | Topic to publish `initial_pose` to (change when targeting non-AMCL localization) |
| `initialpose_retry_interval_sec` | `double` | `0.1` | Timer interval (seconds) for republishing after late-joining subscriber detection. Values below `0.01` are clamped to the default |
| `initialpose_retry_max_attempts` | `int` | `50` | Maximum number of wait attempts before subscriber detection (gives up after `interval_sec × max_attempts` seconds). Default = about 5 seconds |
| `initialpose_post_subscribe_republish_count` | `int` | `3` | Number of additional republishes after subscriber detection. Prevents AMCL from missing a message in the "visible but not quite ready to process" window |
| `map_frame` | `string` | `map` | `PoseWithCovarianceStamped.header.frame_id` |

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest` | The POI name to adopt as the initial pose (`{map_name, poi_name}`). Forwards the pose resolved via `mapoi/get_pois_info` to `/initialpose`. An empty `poi_name` is ignored |

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `initialpose` | `geometry_msgs/PoseWithCovarianceStamped` | Publishes the initial pose (the topic name can be changed via the `initial_pose_topic` parameter). Forwards the pose of the POI received on `mapoi/initialpose_poi`. Uses a wall_timer-based asynchronous retry + post-subscribe republish (the `initialpose_retry_*` parameters, #152) to handle late-joining subscribers / the just-before-ready detection window |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | Publishes the localization bridge's readiness summary at 1Hz (#209). `transient_local` QoS. A minimal 3-field message (`backend_type` / `backend_ready` / `reason`). `backend_ready` uses the `/initialpose` subscriber count > 0 as a readiness proxy. The WebUI / panel gate the Initial Pose UI based on `backend_ready` |

#### Service clients

| Service | Type | Description |
| --- | --- | --- |
| `mapoi/get_pois_info` | `GetPoisInfo` | Calls `mapoi_server` to resolve a pose from a POI name |

#### Requirements for a compatible localization package

`mapoi_amcl_localization_bridge` works with any localization package that satisfies:

- Can receive `geometry_msgs/PoseWithCovarianceStamped` over a topic (default `/initialpose`)
- Accepts an initial pose via that topic dynamically (i.e. after an operator map switch post-startup)

**Verified to work with**: Nav2 AMCL (Humble / Jazzy).

**For packages using a different topic** (e.g. an in-house implementation), you can change the publish target via the `initial_pose_topic` parameter. If the message type it receives isn't `PoseWithCovarianceStamped`, stop this bridge and provide your own bridge satisfying the same spec (see the "Localization backend" section of [docs/backend-status.md](../docs/backend-status.md)).

**Note**: since map swaps via `/mapoi/nav/switch_map` go through the Nav2 `LoadMap` service, localization that isn't on the Nav2 lifecycle will need separate verification and handling to keep map swaps consistent.

### mapoi_rviz2_publisher

A node for displaying POI markers in RViz2.

#### Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `show_tolerance_sector` | `bool` | `true` | Whether to show the POI tolerance visualization (#136 / #179). Covers the following layers: the xy-detection-circle outline + the yaw-constraint sector (drawn only when `0 < tolerance.yaw < π`) + the pause overlay (a dot pattern along the xy circle). For `tolerance.yaw >= π` (yaw doesn't matter), a filled circle is drawn instead of a sector (#267). `false` suppresses all 3 layers for every POI (useful for editor-focused workflows or when RViz feels too cluttered). The WebUI has an equivalent rendering spec (`mapoi_webui/web/js/map-viewer.js`), so update both in tandem when changing this behavior |
| `poi_label_format` | `string` | `"index"` | Display format for the POI label: `"index"` = POI Editor row number (1-based, sequential, independent of tag filtering) / `"name"` = POI name / `"both"` = `"<index>: <name>"` / `"none"` = hidden |
| `route_display_mode` | `string` | `"selected"` | Display format for route markers: `"all"` = show all routes (the active route is emphasized with a thicker, opaque line) / `"selected"` = show only the active route / `"none"` = don't display |

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/highlight/goal` | `std_msgs/String` | Name of the goal POI to highlight |
| `mapoi/highlight/route` | `std_msgs/String` | Names of the route POIs to highlight (comma-separated) |

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/markers/pois` | `visualization_msgs/MarkerArray` | POI markers. `waypoint` is drawn green, `landmark` gray, and a POI with only custom tags is drawn blue |
| `mapoi/markers/routes` | `visualization_msgs/MarkerArray` | Route markers. Draws the route line and a direction arrow for the selected route |

`mapoi/markers/pois` separates layers by Marker namespace. You can toggle visibility per layer using RViz's `Namespaces` toggle.

| Marker namespace | Description |
| --- | --- |
| `arrow/<poi_name>` | The POI's direction arrow and label |
| `tolerance_xy/<poi_name>` | The `tolerance.xy` detection circle |
| `tolerance_yaw/<poi_name>` | The `tolerance.yaw` sector |
| `status_paused/<poi_name>` | The dot overlay for a `pause`-tagged POI |

## Tag system

POIs can be tagged to classify their purpose. There are two kinds of tags: **system tags** and **user tags**.

### System tags

Global tags defined as compile-time constants in `include/mapoi_server/system_tags.hpp` (`mapoi::kSystemTags`). Served via the `mapoi/get_tag_definitions` service. Since system tags are tied to core functionality, changing them is discouraged, and adding/removing one is treated as a core modification.

| Tag | Description |
| --- | --- |
| `waypoint` | A Nav2 navigation target (works both as a single-shot navigation goal and as a route mid-point) |
| `landmark` | A reference-only POI excluded from Nav2 navigation targets (visualization only; excluded from waypoint candidates and initial-pose candidates) |
| `pause` | When the robot enters the POI's radius, it automatically pauses **if it's during ROUTE navigation and the POI is included in the active route's `waypoints` / `landmarks`** (#143). Does not fire during GOAL navigation or IDLE. Cannot be combined with `landmark` |

### User tags

Defined in the `custom_tags` section of each map's `mapoi_config.yaml`.

```yaml
custom_tags:
  - name: audio_info
    description: "音声ガイドのトリガー"
```

## Config file (mapoi_config.yaml)

Place a `mapoi_config.yaml` in each map directory to configure the map and its POIs.

```yaml
custom_tags:
  - name: audio_info
    description: "音声ガイドのトリガー"

map:
  path_planning: map_file_name
  localization: map_file_name

poi:
  # POI names below are generic examples showing the structure (independent of the actual turtlebot3 demo POI names)
  - name: entrance
    pose: {x: -2.0, y: -0.5, yaw: 0.0}
    tolerance: {xy: 0.5, yaw: 0.785}
    tags: [waypoint]                # the first POI in the list is adopted as the default initial pose (#144)
    description: エントランス
  - name: checkpoint_a
    pose: {x: 0.5, y: 0.5, yaw: 0.0}
    tolerance: {xy: 0.5, yaw: 0.785}
    tags: [waypoint, pause]   # auto-pauses on entering the tolerance.xy radius
    description: 中間チェックポイント
  - name: info_point_a
    pose: {x: 1.0, y: 2.0, yaw: 1.57}
    tolerance: {xy: 1.0, yaw: 0.785}
    tags: [audio_info]
    description: 音声案内ポイントA

route:
  - name: route_1
    waypoints: [entrance, info_point_a]
```

### Field descriptions

- **custom_tags**: user-defined tags
  - `name`: tag name
  - `description`: description of the tag
- **map**: settings for the map files in use
  - `path_planning`: map filename used for path planning
  - `localization`: map filename used for localization
- **poi**: POI definitions (**order carries semantics**: for each map, the **first POI** listed under `poi:` is adopted as the default initial pose on map load/switch. The first entry must have no `landmark` tag and complete `pose.x/y/yaw` fields. To specify it explicitly instead, pass a POI name via `SelectMap.srv`'s `initial_poi_name`, #144)
  - `name`: the POI's name (used when specifying it in a topic)
  - `pose`: position (`x`, `y`, `yaw` are all required. Cannot be missing if this POI may be used as a default initial-pose candidate)
  - `tolerance`: Nav2 alignment tolerance struct
    - `xy`: Euclidean tolerance (m). Also used as the POI entry-detection radius
    - `yaw`: Angular tolerance (rad). `0` = unspecified, falls back to the Nav2 default
  - `tags`: list of tags
  - `description`: description text
- **route**: route definitions
  - `name`: route name
  - `waypoints`: list of POI names to visit in order (sends `waypoint`-tagged POIs to Nav2's `FollowWaypoints`)
  - `landmarks` (optional): list of auxiliary POI names to watch during this route (#143). Not sent to Nav2; used only for radius-event monitoring and the pause-trigger scope. Intended for `landmark`-tagged POIs
- **gazebo** (optional, referenced only by `mapoi_gazebo_bridge`): Gazebo Classic model definition swapped in on an operator map switch
  - `world_model.uri`: model URI (e.g. `model://turtlebot3_world`)
  - `world_model.name`: the entity name inside Gazebo (the key used for delete/spawn)

## Directory structure

Under `maps_path` is a subdirectory per map (system tag definitions are held as constants in `include/mapoi_server/system_tags.hpp`, so they are not part of this directory).

```
maps/
└── <map_name>/
    ├── mapoi_config.yaml    # POI, route, and user-tag configuration
    ├── <map_name>.yaml      # map metadata
    └── <map_name>.pgm       # map image
```
