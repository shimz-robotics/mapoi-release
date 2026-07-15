# mapoi_webui

> Japanese version: [README.ja.md](./README.ja.md)

The Web UI package for mapoi.
From a browser, you can view the map, edit POIs, check routes, operate navigation, and monitor the robot's position.
It also works from smartphones and tablets.

## Features

- **Map display**: shows the occupancy grid map in the browser, supports switching maps; zoom is via mouse wheel, drag, and pinch only (no +/− buttons)
- **POI editing**: add, edit, delete, and save POIs (automatically notifies `mapoi_server` to reload). Select a POI on the map with a single click, open the edit panel with a double click, toggle the position-lock (locked by default) to drag the selected POI, and drag the tip handle of the sector (yaw constraint) to rotate its orientation (yaw) (both are committed via Save). Editing supports Undo/Redo (Ctrl+Z / Ctrl+Shift+Z); on mobile, a floating Undo button on the map lets you revert the last operation without opening the panel
- **Route display**: shows routes as polylines with arrow markers, with a show/hide toggle
- **Navigation operation**: goal navigation to a POI, route navigation, pause/resume, cancel
- **Mobile display**: opens with Navigation prioritized, with the map taking up about half the screen
- **Navigation backend detection**: shows the Navigation connected / unavailable badge in the UI, driven by the `mapoi/nav/backend_status` readiness topic (#198). Detection from the command topics' subscriber counts remains only as the best-effort fallback in the capabilities payload (`/api/mode` and the `navigation` field of `/api/nav/status`), used when the readiness topic has never been received
- **Localization backend detection**: shows the Localization connected badge and gates the Set Initial Pose UI, driven by the `mapoi/localization/backend_status` readiness topic (#209)
- **Localization reset**: sets the Initial Pose by selecting a POI
- **Robot position display**: real-time robot position marker via TF (`map` → `base_link`)
- **Tag system**: displays system tags and user tags, color-codes POIs by tag
- **UI display settings**: toggles to show/hide the overall UI (header, POI panel), and a semi-transparent overlay mode that lays the POI panel over the map (opacity adjustable, setting persists)

## Nodes

### mapoi_webui_node

A ROS2 node with a built-in Flask-based HTTP server.

#### Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `maps_path` | `string` | `""` | Path to the maps directory |
| `map_name` | `string` | `turtlebot3_world` | Name of the map to load at startup |
| `config_file` | `string` | `mapoi_config.yaml` | Name of the config file |
| `web_port` | `int` | `8765` | Port for the HTTP server |
| `web_host` | `string` | `0.0.0.0` | Bind address for the HTTP server |
| `map_frame` | `string` | `map` | Parent TF frame |
| `base_frame` | `string` | `base_link` | Child TF frame |
| `robot_radius` | `double` | `0.15` | The robot's physical size (m). Used for the frontend robot marker size and the arrival threshold of the active-route connector. Has the same meaning as Nav2's `robot_radius`, but is kept as an independent param since this node is also meant to work without Nav2. When used together with Nav2, keep the two values in sync (#117) |

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | Publishes the goal POI name |
| `mapoi/nav/route` | `std_msgs/String` | Starts route navigation |
| `mapoi/nav/pause` | `std_msgs/String` | Pauses navigation |
| `mapoi/nav/resume` | `std_msgs/String` | Resumes navigation |
| `mapoi/nav/cancel` | `std_msgs/String` | Cancels navigation |
| `mapoi/nav/switch_map` | `std_msgs/String` | Navigation map-switch instruction. Received by a navigation backend such as `mapoi_nav2_bridge`, which performs the actual map switch |

#### Service clients

| Service | Type | Description |
| --- | --- | --- |
| `mapoi/request_initial_pose` | `RequestInitialPose` | Asks `mapoi_server` to publish upon receiving `POST /api/nav/initial-pose` (#211). The WebUI does not publish `mapoi/initialpose_poi` directly |

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/nav/status` | `std_msgs/String` | Receives navigation status (`"status"` / `"status:target"` format, transient_local QoS). Splits on `:` to extract the target and exposes it as the `target` field of REST `/api/nav/status` |
| `mapoi/nav/command_rejected` | `std_msgs/String` | Receives the reject-command event notification (volatile QoS, payload = target string, #354). A thin conversion that forwards it to the frontend as an SSE `command_rejected` event, without touching the latched snapshot of `mapoi/nav/status` |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | Receives the navigation backend readiness summary published at 1Hz by `mapoi_nav2_bridge` (#198). Drives the Navigation connected badge and gates the navigation operation UI. QoS: transient_local + liveliness (lease 5s, #208); if the publisher dies, the UI is disabled |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | Receives the localization backend readiness summary published at 1Hz by `mapoi_amcl_localization_bridge` (or a custom localization bridge) (#209). Drives the Localization connected badge and gates the Set Initial Pose UI. Same QoS as `mapoi/nav/backend_status` |
| `mapoi/config_path` | `std_msgs/String` | Detects an external map switch |

#### TF

| Transform | Description |
| --- | --- |
| `map` → `base_link` | Gets the robot's position (5Hz) |

## REST API

Ahead of v1.0.0, the URL hierarchy is split into editor-side (`/api/editor/*`) and nav-side (`/api/nav/*`) (#340). The **"Acts on" column** indicates the scope each endpoint touches:

- **Edit context only**: only changes the edit target (map context / YAML) inside `mapoi_server`, with no effect whatsoever on the running real robot or Nav2
- **Affects the real robot**: sends instructions to Nav2 / the navigation backend (`mapoi_nav2_bridge` etc.), changing the behavior (navigation, map switch, localization) of the running robot
- **Read-only**: only retrieves state, with no side effects

| Method | Endpoint | Acts on | Description |
| --- | --- | --- | --- |
| GET | `/api/maps` | Read-only | List of maps and the current map name |
| GET | `/api/maps/<name>/image` | Read-only | Map image (PNG) |
| GET | `/api/maps/<name>/metadata` | Read-only | Map metadata (resolution, origin, size) |
| POST | `/api/editor/select-map` | Edit context only (no effect on the real robot) | Switches `mapoi_server`'s edit-target map (the `select_map` service). Has no effect on Nav2 / the real robot's navigation map. Calling this alone can leave the server's edit context diverged from Nav2's actual map (`/api/nav/switch-map` resolves the divergence) |
| GET | `/api/pois` | Read-only | List of POIs |
| POST | `/api/pois` | Edit context only | Saves POIs |
| GET | `/api/routes` | Read-only | List of routes |
| POST | `/api/routes` | Edit context only | Saves routes |
| GET | `/api/tag-definitions` | Read-only | List of tag definitions |
| POST | `/api/custom-tags` | Edit context only | Saves custom tags |
| GET | `/api/nav/status` | Read-only | Navigation status, robot position, and `robot_radius` (m) |
| POST | `/api/nav/goal` | Affects the real robot | Goal navigation to a POI |
| POST | `/api/nav/route` | Affects the real robot | Starts route navigation |
| POST | `/api/nav/pause` | Affects the real robot | Pauses navigation |
| POST | `/api/nav/resume` | Affects the real robot | Resumes navigation |
| POST | `/api/nav/cancel` | Affects the real robot | Stops navigation |
| POST | `/api/nav/initial-pose` | Affects the real robot | Resets localization |
| POST | `/api/nav/switch-map` | Affects the real robot | Actually switches the running robot's Nav2 map. Publishes the map name to `mapoi/nav/switch_map` |
| GET | `/api/mode` | Read-only | Navigation feature detection result (`navigation_available`, topic subscriber count) |

> **The difference between `/api/editor/select-map` and `/api/nav/switch-map` (must read)**: despite the similar names, their behavior is completely different. `/api/editor/select-map` only switches which map's POIs/Routes/CustomTags the WebUI is editing, and never touches Nav2 or the real robot's navigation map. `/api/nav/switch-map` actually switches the running robot's Nav2 map. If you call only `/api/editor/select-map`, the server's edit context can end up diverged from Nav2's actual running map — if you want both to match, the caller needs to call both (#340).

> **Note (callers of `POST /api/nav/*` must read)**: publishing to a `mapoi/nav/*` topic only tells you whether the local publish succeeded, not whether the navigation backend (`mapoi_nav2_bridge` etc.) actually received and executed the command. Because of this, `POST /api/nav/goal` `/route` `/pause` `/resume` `/cancel` `/initial-pose` `/switch-map` all return HTTP `200 OK` even on a failure such as no subscriber present, and instead put the reason in the response body's `warning` field (see "REST API and server dependency" below for details). **The HTTP status code alone cannot detect failure** — external clients must always check whether the response body contains a `warning` field.

### Error responses

Endpoints that return 4xx/5xx include a machine-readable `code` field alongside the human-readable `error` field (#343). Since the wording of `error` may change, client-side branching should use `code` (a 200 + `warning` is not an error, so it has no `code` — see the note above).

| `code` | HTTP status | Meaning |
| --- | --- | --- |
| `invalid_request` | 400 | Malformed request body, missing required field, invalid value |
| `not_found` | 404 | The specified map / config (yaml) doesn't exist |
| `version_mismatch` | 409 | Optimistic-concurrency conflict detected (`expected_version` mismatch, see below) |
| `service_unavailable` | 503 | A dependent ROS 2 service is unavailable / timed out |
| `internal_error` | 500 | An unexpected server-side exception (e.g. a YAML write failure) |

### Optimistic concurrency detection (`expected_version`)

Since `POST /api/pois` `/api/routes` `/api/custom-tags` all write to the same yaml (`mapoi_config.yaml`), they share a common optimistic-concurrency mechanism (the `expected_version` field; added to `/api/pois` in #241, and extended to `/api/routes` `/api/custom-tags` in #343). If you send back the `config_version` (a sha256 hash of the yaml contents) returned by the corresponding GET (`/api/pois` `/api/routes` `/api/tag-definitions`) as `expected_version` on POST, the backend compares it against the current yaml and, on a mismatch, returns `409` + `code: version_mismatch`. Through the WebUI, the frontend handles this automatically (reloading after a confirmation dialog); for external POSTs (curl / another client), omitting `expected_version` skips the check, so if you want to avoid clobbering concurrent edits, it's the caller's responsibility to send back the corresponding GET's `config_version`.

Since `config_version` is a content hash of the entire yaml file, saving any one of POI/Route/CustomTags also changes the `config_version` returned by the other two GETs (this is intentional, since they share the same yaml). See the implementation comments (`api_save_pois` / `api_save_routes` / `api_save_custom_tags`) / test (`test_api_save_pois_version_conflict.py`) for the detailed spec.

### SSE (`GET /api/events`)

A server-sent events endpoint that the frontend connects to via `EventSource` (#135 (B)). Used to push events that are "hard to catch via polling / want to notify without polluting the status snapshot," such as config changes from rviz / an external save, or a reject while navigating. Each event is JSON in the form `{"type": "<event>", "payload": {...}}` (`payload` may be omitted).

| `type` | `payload` | Fired when |
| --- | --- | --- |
| `config_changed` | `{"map_name": "<name>"}` | On receiving `mapoi/config_path` (a POI/Route/CustomTags change from rviz / an external save, or an external map switch). The frontend debounces 200ms and re-runs `loadPois` / `loadRoutes` / `loadTagDefinitions` |
| `command_rejected` | `{"target": "<string>"}` | On receiving `mapoi/nav/command_rejected` (#354). An event notification for something like a typo'd goal while navigating, conveyed without polluting `mapoi/nav/status`. The frontend shows a transient toast (`Command rejected: <target>`, auto-dismissing after about 5 seconds) |

The connection interleaves a 30-second heartbeat comment (`:heartbeat`), and client disconnects are automatically discarded server-side (see the implementation comments in `_broadcast_sse_event` / `/api/events` for details).

## Launch methods (3 scenarios)

Use one of 3 launch methods depending on your purpose. All of them are accessed via a browser at `http://<host>:8765`.

### A. webui only (for operators, with `mapoi_server` already running as a separate process)

For adding webui as a replacement for RViz when the robot is already running (`mapoi_server` / `mapoi_nav2_bridge` etc. running as separate processes).

```bash
ros2 launch mapoi_webui mapoi_webui.launch.yaml maps_path:=/path/to/maps map_name:=your_map
```

### B. webui + mapoi_server together (for editing)

For editing POIs/Routes/CustomTags on a desktop PC only (no robot operation / Nav2 / RViz needed). Includes `mapoi_bringup` with a minimal config (`with_nav2_bridge=false`, `with_rviz_publisher=false`, `simulator=none`).

```bash
ros2 launch mapoi_webui mapoi_editor.launch.yaml maps_path:=/path/to/maps map_name:=your_map
```

### C. Integrated operation / demo (Nav2 + sim + webui, all together)

Uses an integrated bringup + webui launch such as `mapoi_turtlebot3_example`'s `turtlebot3_navigation.launch.yaml`. See that package's README for details.

## Degraded behavior when `maps_path` is unset

`mapoi_webui_node` starts even when the `maps_path` parameter is `""` (unset). This is asymmetric with `mapoi_server`, which logs FATAL and throws immediately (never starts) when `maps_path` is empty — this is a deliberate design choice. The webui has a legitimate use case (launch method A) of "adding an operator navigation UI as a replacement for RViz onto an environment where the robot is already running," which doesn't need access to a maps directory, so it isn't fail-fast.

Behavior when `maps_path` is unset:

- At startup, it logs `maps_path parameter not set. Set it to use the web editor.` as a `WARN` and continues starting normally
- **The editor features (viewing/editing the map list, map image, POI/Route/CustomTags) are disabled**: `get_maps_list()` returns an empty list when `maps_path` is empty (or a nonexistent directory), so `GET /api/maps` returns `maps: []`, and `GET /api/maps/<name>/image` `/metadata` and the POI/Route/CustomTags endpoints (`GET`/`POST /api/pois` `/api/routes` `/api/custom-tags`) return `404 Not Found` since the target YAML/PNG can't be found
- **The nav-operation features (`/api/nav/*`, `/api/mode`, `GET /api/nav/status`) work independently of `maps_path`**: since these only rely on publishing/subscribing to `mapoi/nav/*` topics and calling `mapoi_server` services, they work normally even without `maps_path` set

So a webui launched without `maps_path` set functions as a "navigation-operation-only add-on UI," with only the map-editing features disabled.

## REST API and server dependency

A quick reference for whether each endpoint requires `mapoi_server` (or `mapoi_nav2_bridge`) to be running:

| endpoint | server required | reason |
|---|---|---|
| `GET /api/maps` `/maps/<name>/image\|metadata` | Not required | Lists `maps_path` directly / reads the YAML/PNG directly |
| `GET /api/pois` `/api/routes` | Not required | Reads the YAML directly |
| `POST /api/editor/select-map` | **Required (`mapoi_server`'s `select_map` service)** | Only switches the edit-target map. Has no effect on Nav2 / the real robot |
| `POST /api/pois` `/api/routes` `/api/custom-tags` | **Required** | Calls the `mapoi/reload_map_info` service after writing the YAML |
| `GET /api/tag-definitions` | **Required** | Via the `mapoi/get_tag_definitions` service |
| `POST /api/nav/{goal,route,cancel,pause,resume}` | **Required (mapoi_nav2_bridge)** | The publisher's listener is mapoi_nav2_bridge; returns a warning if there are 0 subscribers |
| `POST /api/nav/initial-pose` | **Required (`mapoi_server`'s `mapoi/request_initial_pose` service)** | `mapoi_server` publishes `mapoi/initialpose_poi` via the `mapoi/request_initial_pose` service (#211). Returns 503 if the service is absent. Additionally, if there's no subscriber (e.g. `mapoi_amcl_localization_bridge`) on `mapoi/initialpose_poi`, the initial pose isn't delivered and a warning is returned |
| `POST /api/nav/switch-map` | **Required (mapoi_nav2_bridge etc.)** | Actually switches the running robot's Nav2 map. The `mapoi/nav/switch_map` publisher's listener is the navigation backend; returns a warning if there are 0 subscribers |
| `GET /api/nav/status` | Not required (returns the value received via subscriber) | Returns default values if mapoi_nav2_bridge is absent |
| `GET /api/mode` | Not required | Best-effort detection from the command topic's subscriber count |

Behavior when server / mapoi_nav2_bridge is absent (by endpoint category):
- `POST /api/editor/select-map`: `503 Service Unavailable` (service required; when unavailable / timed out)
- `GET /api/tag-definitions`: `503 Service Unavailable` (service required; when unavailable / timed out)
- `POST /api/pois` / `/api/routes` / `/api/custom-tags`: `200 OK` + `warning` field (the YAML write itself succeeds, so an unavailable / timed-out / failed `mapoi/reload_map_info` is reported as a warning)
- `POST /api/nav/{goal,route,cancel,pause,resume}` / `/api/nav/initial-pose` / `/api/nav/switch-map`: `200 OK` + `warning` field (the publish itself is treated as successful; absence of a subscriber is detected best-effort and reported)

## Developer conventions

### Calling a ROS 2 service from a Flask thread

**Always go through `MapoiWebNode._call_service_sync()`**. Fire-and-forgetting `client.call_async()` directly, or calling `spin_until_future_complete()` from a Flask thread, is prohibited (the former silently fails, and the latter races with the main thread's `rclpy.spin` executor).

```python
response = node._call_service_sync(
    node.some_client_, SomeReq(), 'some_service', timeout_sec=3.0)
if response is None:
    return jsonify({'error': 'service unavailable'}), 503
```

### Publishing a ROS 2 message from a Flask thread

**Always publish via `MapoiWebNode.publish_with_subscriber_check()`**, to avoid silent failures when there are 0 subscribers (e.g. `mapoi_nav2_bridge` not running).

```python
_, warning = node.publish_with_subscriber_check(
    node.some_pub_, msg, 'some_topic')
return jsonify({'success': True, 'warning': warning} if warning else {'success': True})
```

## Dependencies

- `rclpy`
- `std_msgs`, `std_srvs`
- `mapoi_interfaces`
- `tf2_ros`
- `python3-flask`
- `python3-pil`
- `python3-yaml`
