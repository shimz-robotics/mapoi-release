# mapoi_interfaces

> Japanese version: [README.ja.md](./README.ja.md)

Package defining the messages and services used across the mapoi packages.

## Messages (msg)

### Tolerance.msg

Tolerance for POI arrival detection (aligned with Nav2 `SimpleGoalChecker`'s `xy_goal_tolerance` / `yaw_goal_tolerance`).

| Field | Type | Description |
| --- | --- | --- |
| `xy` | `float64` | Euclidean tolerance (m). Also used as the POI entry-detection radius |
| `yaw` | `float64` | Angular tolerance (rad). `0` means unspecified and falls back to the Nav2 default |

### PointOfInterest.msg

Message representing a POI (Point of Interest).

| Field | Type | Description |
| --- | --- | --- |
| `name` | `string` | POI name. Serves as the effective unique key |
| `pose` | `geometry_msgs/Pose` | Position and orientation of the POI |
| `tolerance` | `mapoi_interfaces/Tolerance` | xy / yaw tolerance (replaced the old `radius` field in v0.3.0) |
| `tags` | `string[]` | Tags associated with the POI (e.g. system tags `waypoint` / `landmark` / `pause`, or a user-defined tag such as `audio_info`) |
| `description` | `string` | Description of the POI |

### PoiEvent.msg

Event message published when a robot enters/exits a route-registered POI during route navigation, or when navigation stops at a `pause`-tagged POI (#220 simplified this from 4 event types to 3).

| Field | Type | Description |
| --- | --- | --- |
| `EVENT_ENTER` | `uint8` (constant=1) | The robot entered a route-registered POI's `tolerance.xy` radius during route navigation (only when `nav_mode == ROUTE` and the POI is in `current_route_poi_names_`; not published outside route navigation, i.e. in IDLE / GOAL mode) |
| `EVENT_PAUSED` | `uint8` (constant=2) | Navigation stopped (detected via cmd_vel dwell) within the `tolerance.xy` of a `pause`-tagged POI. Fires the moment Nav2 enters the stopped state after the pause auto-trigger |
| `EVENT_EXIT` | `uint8` (constant=3) | The robot exited a route-registered POI beyond `tolerance.xy * hysteresis_exit_multiplier` |
| `event_type` | `uint8` | Event type (one of the 3 above) |
| `poi` | `mapoi_interfaces/PointOfInterest` | Information about the target POI |
| `stamp` | `builtin_interfaces/Time` | Timestamp of the event |

Lifecycle:
```
EVENT_ENTER -> [EVENT_PAUSED (only if pause-tagged + nav stops)] -> EVENT_EXIT
```

Prerequisites for `EVENT_PAUSED`:
- The controller in use must **keep publishing cmd_vel = 0 while navigation is stopped** (the Nav2 default behavior). If the controller stops publishing cmd_vel while stationary, `EVENT_PAUSED` will not fire.
- The pause auto-trigger (and the `EVENT_PAUSED` publish) is performed on the `mapoi_nav2_bridge` side, and resume is triggered by a client-side `mapoi/nav/resume` request, so there is no `RESUMED`-equivalent event in this spec (resume can be observed via the status topic).

### TagDefinition.msg

Message representing a single POI classification tag definition (system tag / user tag). Introduced in PR #193 so it can also be used as a response component of `GetTagDefinitions.srv`.

| Field | Type | Description |
| --- | --- | --- |
| `name` | `string` | Tag name (e.g. system tags `waypoint` / `landmark` / `pause`, or a user-defined tag such as `audio_info`) |
| `description` | `string` | Human-readable description of the tag's purpose |
| `is_system` | `bool` | `true` = system tag, `false` = user-defined tag |

### InitialPoseRequest.msg

Notification message for the "candidate initial-pose POI" accompanying an operator's map switch / reload (typed in #149 round 8, previously a pair of strings). Published by `mapoi_server`, the topic's sole writer, on request via the `mapoi/request_initial_pose` service (#211) — e.g. `mapoi_nav2_bridge` requests it after a successful Nav2 `LoadMap` — as well as on its own at startup (default POI adoption) and on reload (stale clear), and subscribed to by the localization bridges. See the header comment in `msg/InitialPoseRequest.msg` for details and the stale-rejection strategy.

| Field | Type | Description |
| --- | --- | --- |
| `map_name` | `string` | Name of the map this notification targets (for generation identification). If non-empty, it must match the current map at publish time (#299). Empty is only possible when a requester with an unknown map passes an empty value to the service |
| `poi_name` | `string` | Name of the POI to adopt (empty string = "no candidate", ignored by subscribers) |

QoS is `transient_local` (depth=1), so late-joining subscribers can also receive the latched value.

### NavigationBackendStatus.msg

Readiness message published at 1 Hz to `mapoi/nav/backend_status` by the navigation bridge (Nav2 bridge / custom bridge). The UI side (`mapoi_webui`, `mapoi_rviz_plugins`) gates navigation operations based on `backend_ready`.

| Field | Type | Description |
| --- | --- | --- |
| `backend_type` | `string` | Bridge identifier (e.g. `nav2`, `custom_lidar_planner`). For tooltip display |
| `backend_ready` | `bool` | Whether the bridge can accept and execute navigation commands |
| `reason` | `string` | Human-readable reason when `backend_ready=false` (optional, must not contain sensitive information — see below) |

#### QoS contract (issue #208)

Both the publisher and subscriber must always specify the following:

- `durability`: `TRANSIENT_LOCAL` (so a late-joining UI can receive the latest status)
- `reliability`: `RELIABLE`
- `liveliness` (publisher): `MANUAL_BY_TOPIC` (asserts liveliness on every `publish()`)
- `liveliness` (subscriber): `AUTOMATIC` (only `MANUAL_BY_TOPIC` publisher × `AUTOMATIC` subscriber is compatible)
- `liveliness_lease_duration`: 5 s (both sides; must satisfy `pub.lease <= sub.lease`)

See the header comment in `msg/NavigationBackendStatus.msg` for details and violation patterns.

#### Guidance for custom bridge implementers (issue #207)

Each bridge should compute `backend_ready` as the AND of the capabilities it *actually* exposes. `mapoi_nav2_bridge`'s (Nav2 bridge) `goal_ready && route_ready && switch_map_ready` is **only correct for a bridge that exposes all 3 capabilities**. If a bridge that only exposes a single capability (e.g. NavigateToPose only) mimics this, the capability it doesn't expose is always false → `backend_ready` is also always false → the UI always shows "Navigation unavailable".

See the header comment in `msg/NavigationBackendStatus.msg` for concrete examples and the field-population conventions (including the `reason` phrasing convention).

`reason` is shown in the operator UI and can be recorded in bags or remote dashboards, so it must not contain sensitive information such as credentials, tokens, absolute paths, internal hostnames, IP addresses, user identifiers, or stack traces. Keep it to a capability name plus a short state verb (e.g. `not ready: navigate_to_pose action`).

> **Scope of the CI lint** (`scripts/check_docs_consistency.py`, PR #217 / Closes #216): the static check only scans **explicit string literals** in `publish_backend_status`-family functions. Dynamically-assembled strings (e.g. `reason = "ip=" + this->get_parameter(...).as_string()`) and raw string literals (`R"(...)"`) are out of scope. Passing the lint does *not* guarantee complete redaction. Bridge implementers must apply this section's prohibitions to dynamic parts as well.

### LocalizationBackendStatus.msg

Readiness message published at 1 Hz to `mapoi/localization/backend_status` by the localization bridge (`mapoi_amcl_localization_bridge` / a custom bridge) (#209). The UI side gates Initial Pose operations based on `backend_ready`. This is an independent axis from the Navigation backend (`NavigationBackendStatus` above) and each is treated as a separate indicator.

| Field | Type | Description |
| --- | --- | --- |
| `backend_type` | `string` | Bridge identifier (e.g. `amcl`, `slam_toolbox`, `custom_lidar_amcl`). For tooltip display |
| `backend_ready` | `bool` | Whether the bridge can accept and forward a new initial pose |
| `reason` | `string` | Human-readable reason when `backend_ready=false` (optional, sensitive information prohibited — same policy as the `NavigationBackendStatus` section) |

The QoS contract (TRANSIENT_LOCAL / RELIABLE / MANUAL_BY_TOPIC publisher / AUTOMATIC subscriber / 5s lease) is identical to `NavigationBackendStatus`. See the header comment in `msg/LocalizationBackendStatus.msg` for details.

## Services (srv)

### SelectMap.srv

A Nav2-free service for switching the current map context. In operator mode, a map switch is requested via the `/mapoi/nav/switch_map` topic, and `mapoi_nav2_bridge` calls this service before executing Nav2's `LoadMap`.

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Request | `map_name` | `string` | Name of the map to select |
| Request | `initial_poi_name` | `string` | POI name to use for the initial pose. If empty, the first valid POI is used |
| Response | `success` | `bool` | Whether the context selection succeeded |
| Response | `error_message` | `string` | Reason for failure |
| Response | `config_path` | `string` | Path of the config file after selection |
| Response | `resolved_initial_poi_name` | `string` | The resolved initial-pose POI name |
| Response | `nav2_node_names` | `string[]` | Nav2 map server node names |
| Response | `nav2_map_urls` | `string[]` | Corresponding map YAML paths |

### RequestInitialPose.srv

A service that requests a publish to `mapoi/initialpose_poi` (whose sole writer is `mapoi_server`) (#211). Previously, `mapoi_server` / `mapoi_nav2_bridge` / WebUI / the RViz panel all published directly, but since `transient_local`'s latched cache is kept per writer, this caused stale conflicts across writers. Consolidating all publishing through this service into a single `mapoi_server` unifies the latched cache, so that a clear takes effect as a true last-write-wins.

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Request | `map_name` | `string` | Target map name (mirrors `InitialPoseRequest.msg`'s `map_name`). If non-empty, it must match `mapoi_server`'s current map; a mismatch means nothing is published and `success=false` (#299). Empty passes through without validation, for a requester with an unknown map |
| Request | `poi_name` | `string` | POI name to adopt. An empty string means clear (publishes a skip sample, ignored by subscribers) |
| Response | `success` | `bool` | `true` if the publish succeeded (`false` if rejected due to a `map_name` mismatch, #299) |
| Response | `error_message` | `string` | Reason for failure (non-empty) |

### GetMapsInfo.srv

A service to get the list of available maps and the current map name.

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Response | `maps_list` | `string[]` | List of available map names |
| Response | `map_name` | `string` | Current map name |

### GetPoisInfo.srv

A service to get all POIs registered on the current map.

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Response | `pois_list` | `PointOfInterest[]` | List of POIs |

### GetRoutePois.srv

A service to get the POIs included in a given route, separated into navigated waypoints and reference-only landmarks (#143).

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Request | `route_name` | `string` | Route name |
| Response | `success` | `bool` | `true` if the route was found (also `true` when the route exists but has 0 POIs) (#342) |
| Response | `error_message` | `string` | Non-empty only when `success=false`. Explains that the route doesn't exist (#342) |
| Response | `pois_list` | `PointOfInterest[]` | Ordered waypoints on the route (sent to Nav2 as `FollowWaypoints`, or one `NavigateToPose` per waypoint when `waypoint_arrival_mode=mapoi`) |
| Response | `landmark_pois` | `PointOfInterest[]` | Landmarks attached to the route. Not navigated by Nav2, but radius-monitored while the route is active. Order is informational only |

### GetRoutesInfo.srv

A service to get the list of available routes.

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Response | `routes_list` | `string[]` | List of route names |

### GetTagDefinitions.srv

A service to get the tag definitions (system tags / user tags).

| Direction | Field | Type | Description |
| --- | --- | --- | --- |
| Response | `tag_names` | `string[]` | List of tag names |
| Response | `tag_descriptions` | `string[]` | List of tag descriptions |
| Response | `is_system` | `bool[]` | Whether each tag is a system tag (`true` = system tag) |
