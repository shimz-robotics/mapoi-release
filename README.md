# mapoi_rviz_plugins

> Japanese version: [README.ja.md](./README.ja.md)

RViz2 plugin package for mapoi. Provides a GUI for map switching, POI selection, autonomous navigation operation, and POI editing.

## Plugins

### MapoiPanel (panel plugin)

A navigation operation panel that can be added as an RViz2 panel.

- Select a map to switch to from a dropdown
- Select a destination from a list of POIs tagged `waypoint` (POIs also tagged `landmark` are excluded), scoped to the current map
- Select a route from a list and start navigation along it
- Correct the robot's estimated pose to a selected POI's position (setting the Initial Pose)
- Start autonomous navigation to a selected POI
- Pause / resume / cancel autonomous navigation
- Display navigation status (`navigating`, `succeeded`, `aborted`, `canceled`, `paused`, `map_switching`, `map_switch_succeeded`, `map_switch_failed`, `backend_unavailable`, `rejected`; see the `mapoi/nav/status` entry in the [`mapoi_server` README](../mapoi_server/README.md) for the meaning of each value)
- Highlight the selected POI/route in RViz2

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | Publishes the goal POI name. The actual Nav2 action call is handled by the navigation bridge (`mapoi_nav2_bridge` etc.) |
| `mapoi/nav/pause` | `std_msgs/String` | Pauses navigation |
| `mapoi/nav/resume` | `std_msgs/String` | Resumes navigation |
| `mapoi/nav/cancel` | `std_msgs/String` | Cancels navigation |
| `mapoi/nav/route` | `std_msgs/String` | Starts route navigation |
| `mapoi/nav/switch_map` | `std_msgs/String` | Requests a switch to the selected map. The actual map switch is handled by the navigation bridge |
| `mapoi/highlight/goal` | `std_msgs/String` | Highlights the goal marker |
| `mapoi/highlight/route` | `std_msgs/String` | Highlights the route marker |

#### Service clients

| Service | Type | Description |
| --- | --- | --- |
| `mapoi/get_maps_info` | `GetMapsInfo` | Fetches the current map name and the map list for the map dropdown |
| `mapoi/get_pois_info` | `GetPoisInfo` | Fetches the POI list for the destination dropdown |
| `mapoi/get_routes_info` | `GetRoutesInfo` | Fetches the route list for the route dropdown |
| `mapoi/get_route_pois` | `GetRoutePois` | Fetches the POIs of the selected route for highlighting |
| `mapoi/request_initial_pose` | `RequestInitialPose` | Requests `mapoi_server` to publish when setting the Initial Pose (#211). The panel does not publish `mapoi/initialpose_poi` directly |

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi/config_path` | `std_msgs/String` | Detects changes to the config file path |
| `mapoi/nav/status` | `std_msgs/String` | Displays navigation status (`"status"` / `"status:target"` format, transient_local QoS). If a target is included, a late-joining panel can restore messages like "Navigating: target" / "Arrived: target" |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | Enables/disables the navigation controls (Run Goal / Run Route / Pause / Resume / Stop buttons and the map dropdown) based on the navigation backend's readiness and liveliness (#198, #209). QoS follows the msg contract (#208); see [docs/backend-status.md](../docs/backend-status.md) |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | Enables/disables the Initial Pose button based on the localization backend's readiness and liveliness (#209) |

### PoiEditor (panel plugin)

A panel that displays, edits, and saves POI information in tabular form.

- Loads POI information from the current config file
- Switch the map being edited from a dropdown (switches `mapoi_server`'s editing context via `mapoi/select_map`; it does not switch the running robot's map)
- View and edit POI information in a table (5-column layout: name, pose, tolerance, tags, description, #158)
  - The **pose** column uses `x, y, yaw` format (e.g. `1.00, 2.00, 0.79`; x/y in m, yaw in rad, displayed with 2 decimal places)
  - The **tolerance** column uses a single-column combined `xy, yaw` format (e.g. `0.50, 0.79`; xy in m, yaw in rad, displayed with 2 decimal places)
    - Validation constraints: `xy >= 0.001 m` / `0.001 rad <= yaw <= 2π rad`
    - The `2π rad` (= 360°) maximum is a guard that rejects old deg-based inputs (e.g. `45`) mistakenly entered as rad after the deg → rad unit change (#158)
  - **description** is the last column (long text is fine, avoids squeezing the other columns)
- Add / copy / delete POIs
- Undo/redo edits (add/copy/delete rows, cell edits, row reordering) via the Undo/Redo buttons or `Ctrl+Z` / `Ctrl+Shift+Z` (#407, #435)
  - The buttons work regardless of focus; the keyboard shortcuts only work while focus is inside the panel (click a table cell first to enable them)
  - History is cleared when a save succeeds, when the tag filter changes, or when the table is fully rebuilt (e.g. switching maps, or the automatic reload after saving)
- Filter the displayed list by tag
- Tag input assistance via a TagHelperComboBox (system tags shown with an `[S]` marker)
- Save with validation (duplicate-name check, coordinate format, `tolerance.xy` check, warnings for undefined tags)
- Position input in conjunction with MapoiPoseTool
- Automatically reloads `mapoi_server`'s config after saving

#### Service clients

| Service | Type | Description |
| --- | --- | --- |
| `mapoi/select_map` | `SelectMap` | Switches `mapoi_server`'s editing context to the selected map (does not switch the running robot's Nav2 map) |
| `mapoi/get_maps_info` | `GetMapsInfo` | Fetches the current map name and the map list |
| `mapoi/get_pois_info` | `GetPoisInfo` | Fetches the POI list shown in the table |
| `mapoi/get_tag_definitions` | `GetTagDefinitions` | Fetches system/user tag definitions for TagHelperComboBox and save-time validation |
| `mapoi/reload_map_info` | `std_srvs/Trigger` | Asks `mapoi_server` to reload its config after saving |

#### Subscribers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi_rviz_pose` | `geometry_msgs/PoseStamped` | Position input from MapoiPoseTool |
| `mapoi/config_path` | `std_msgs/String` | Detects changes to the config file path |

### MapoiPoseTool (tool plugin)

A pose-specification tool that can be added to the RViz2 toolbar. Shortcut key: `i`

- Click and drag on RViz2 to specify a position and orientation
- Reflects the position onto the selected row in PoiEditor
- Automatically switches back to the default tool after use
- Assumes the RViz2 Fixed Frame is `map`, since POI poses are stored in the `map` frame. If the
  Fixed Frame is something else, the pose is not reflected and a warning dialog is shown instead.

#### Publishers

| Topic | Type | Description |
| --- | --- | --- |
| `mapoi_rviz_pose` | `geometry_msgs/PoseStamped` | Publishes the specified position and orientation |

## Adding to RViz2

1. Launch RViz2
2. Add `MapoiPanel` or `PoiEditor` via Panels > Add New Panel
3. Add `MapoiPoseTool` via the "+" button on the toolbar

## Dependencies

- `rviz_common`
- `rviz_default_plugins`
- `std_srvs`
- `mapoi_interfaces`
