# Navigation / Localization backend specification

> Japanese version: [backend-status.ja.md](./backend-status.ja.md)

mapoi treats the navigation and localization backends as **independent contracts**. A dedicated bridge node is launched for each, and readiness is reported to the WebUI / RViz panel via the `backend_status` topics. To drive your own stack from the mapoi UI, implement your own bridge node that satisfies the topic specifications below.

## Navigation backend specification

`mapoi_nav2_bridge` is effectively the bridge node for Nav2.

**Subscribed command topics** (published by the mapoi UI):

| topic | type |
| --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` (POI name) |
| `mapoi/nav/route` | `std_msgs/String` (route name) |
| `mapoi/nav/cancel` | `std_msgs/String` |
| `mapoi/nav/pause` | `std_msgs/String` |
| `mapoi/nav/resume` | `std_msgs/String` |
| `mapoi/nav/switch_map` | `std_msgs/String` (map name) |

> Receiving `mapoi/initialpose_poi` (`mapoi_interfaces/InitialPoseRequest`) and publishing `/initialpose` were split into `mapoi_amcl_localization_bridge` in #209 and are not part of the Navigation specification. See the "Localization backend specification" section below for details.

**Published status topics** (subscribed by the mapoi UI):

| topic | type |
| --- | --- |
| `mapoi/nav/status` | `std_msgs/String` (`status` or `status:target`) |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` (readiness summary, `transient_local`, minimal 3 fields) |

The `status` values used by `mapoi_nav2_bridge` are `navigating` / `succeeded` / `aborted` / `canceled` / `paused` / `map_switching` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected` (see the `mapoi/nav/status` section of the [`mapoi_server` README](../mapoi_server/README.md) for details). **Mandatory rule for bridge implementations**: on any path where a goal / route / map switch command was "accepted but not executed" (invalid input, internal service not ready, etc.), always publish some terminal status. If you return without publishing, the previous status (`succeeded` / `navigating`, etc.) stays on the WebUI / RViz panel and the operator cannot notice the mistake (#339). Adding new status values is backward compatible for existing subscribers.

**Optional topic**: in addition to the mandatory statuses above, `mapoi_nav2_bridge` always publishes `mapoi/nav/command_rejected` (`std_msgs/String`, volatile QoS, payload is the target only) on every reject (#354). It is an event axis used for WebUI toast notifications, compensating for the constraint that `mapoi/nav/status` is a state snapshot and cannot record a reject while navigating; it is not a mandatory part of a custom bridge (leaving it unimplemented does not affect the `backend_ready` contract).

Distinguishing `rejected` from `backend_unavailable`: reserve `backend_unavailable` for cases where the **Nav2 side** (action server / `/goal_pose` fallback subscriber) is absent and navigation itself cannot be executed. Use `rejected` on paths that reject a command for reasons unrelated to Nav2 readiness — internal service not ready or input validation on the **mapoi_server side**, such as `mapoi/get_pois_info` / `mapoi/get_route_pois` services not ready, a POI name typo, a landmark POI given as a goal, an empty route, etc. (for both goal and route, check readiness with `wait_for_service(2s)` before calling the service, and publish `rejected` if not ready. #339 / #355). Operationally both mean "this command was not executed", but keeping `backend_unavailable` dedicated to Nav2 readiness preserves its correspondence with the WebUI `Navigation connected` badge (`mapoi/nav/backend_status`).

Only while `mapoi/nav/backend_status` reports `backend_ready=true` does the WebUI enter the `Navigation connected` state and enable the navigation UI. The bridge obtains POI / route / map information from the `mapoi_server` services (`mapoi/get_pois_info` / `mapoi/get_route_pois` / `mapoi/select_map`) and translates it into its own navigation API. See the [`mapoi_server` README](../mapoi_server/README.md) for details on each topic / service.

**How to populate each `NavigationBackendStatus` field** (minimal specification):

- `backend_type`: a short string identifying your bridge (e.g. `nav2`, `custom_lidar_planner`). It is only shown in the WebUI tooltip and does not affect behavior, but keep it unique so operators can tell bridges apart in environments where multiple bridges coexist
- `backend_ready`: a boolean indicating whether navigation operations (goal / route / cancel / pause / resume / map switch) can be accepted **right now**. Internally you may AND-combine the readiness of multiple capabilities, or look at only the single capability you need
- `reason`: a human-readable reason string for when `backend_ready=false` (e.g. `"navigate_to_pose action not available"`). An empty string is allowed, but providing a reason is recommended to aid troubleshooting. The WebUI / panel is expected to display it as plain text in a tooltip and does not interpret HTML

The only mandatory obligation for bridge implementers is **to make `backend_ready` true**. If you need a per-capability breakdown, pack it into the `reason` string or publish it on a bridge-specific extra topic.

**Localization is a separate specification**: readiness of self-localization (AMCL / SLAM toolbox / your own localization, etc.) is out of scope here. Subscribing to `mapoi/initialpose_poi` and publishing to `/initialpose` were split into `mapoi_amcl_localization_bridge` in #209 and are defined as the independent `LocalizationBackendStatus` specification (next section). To switch to your own localization, stop the AMCL bridge and have your own bridge publish `mapoi/localization/backend_status` and subscribe to `mapoi/initialpose_poi`.

**Operational note**: since the WebUI / RViz panel enables the operation UI based on `backend_ready=true`, operators are expected to **wait until the `Navigation connected` badge lights up before operating**. For a few hundred milliseconds right after the bridge starts, the action server may still be in discovery and not yet ready; forcing Run during that window can return a one-time `backend_unavailable` status (pressing Run again proceeds normally). The badge display is updated at 1Hz.

## Localization backend specification

`mapoi_amcl_localization_bridge` is the bridge node for AMCL-compatible localization (configurations that receive `/initialpose` as `geometry_msgs/PoseWithCovarianceStamped`). To drive slam_toolbox / NDT / your own localization, etc. from the mapoi UI (WebUI / RViz panel), implement your own bridge node that satisfies the topic specification below. It is operated as a specification independent of the Navigation backend, so both bridges can run at the same time.

**Subscribed command topics** (published by `mapoi_server`, the topic's sole writer — the WebUI / RViz panel / `mapoi_nav2_bridge` trigger it via the `mapoi/request_initial_pose` service, #211):

| topic | type |
| --- | --- |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest` (`{map_name, poi_name}`, `transient_local`) |

**Published status topics** (subscribed by the mapoi UI):

| topic | type |
| --- | --- |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` (readiness summary, `transient_local`, minimal 3 fields) |

Only while `mapoi/localization/backend_status` reports `backend_ready=true` does the WebUI / RViz panel enable the Initial Pose UI. The bridge obtains POI information from the `mapoi_server` `mapoi/get_pois_info` service, resolves `InitialPoseRequest.poi_name`, and forwards it to its own localization.

**How to populate each `LocalizationBackendStatus` field** (minimal specification):

- `backend_type`: a short string identifying your bridge (e.g. `amcl`, `slam_toolbox`, `custom_lidar_amcl`). It is only shown in the WebUI tooltip and does not affect behavior, but keep it unique so operators can tell bridges apart in environments where multiple bridges coexist
- `backend_ready`: a boolean indicating whether an initial pose can be accepted **right now** — the downstream localization, as seen from the bridge, is in a listening state (`mapoi_amcl_localization_bridge` uses subscriber count > 0 on `/initialpose` as its ready condition)
- `reason`: a human-readable reason string for when `backend_ready=false` (e.g. `"no subscriber on /initialpose (localization node not running?)"`). An empty string is allowed, but providing a reason is recommended to aid troubleshooting

The only mandatory obligation for bridge implementers is **to make `backend_ready` true**. Thanks to the independence from the Navigation axis, a localization-only configuration without Nav2 (POI editor + AMCL only) is also viable.

**Relationship to the Navigation backend**: the WebUI / RViz panel **displays the two badges as separate indicators** (`Navigation connected` / `Localization connected`). Nav2 can go down while AMCL stays ready, and vice versa. An operator map switch (`mapoi/nav/switch_map`) proceeds through the sequence Nav2 LoadMap → mapoi/initialpose_poi publish → localization bridge resolve, but the UI **gates the map switch button on navigation backend_ready only** (even if the localization bridge is absent / temporarily unreachable, the localization bridge's late-subscriber retry absorbs the initial pose delivery). To make sure the initial pose goes through, the operator should visually confirm both badges before executing the map switch.
