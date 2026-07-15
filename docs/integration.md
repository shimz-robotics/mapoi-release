# Integrating with your own robot

> Japanese version: [integration.ja.md](./integration.ja.md)

For now, clone this repository and build it with colcon. **Distribution via apt/rosdep is planned** ([#20](https://github.com/shimz-robotics/mapoi/issues/20)).

See [docs/architecture.md](./architecture.md) for the big picture of how the packages and nodes fit together.

## 1. Creating the map directory

Create a `maps/<map_name>/` directory inside your package and place the map files and the config file there.

```
maps/
├── site_a/
│   ├── mapoi_config.yaml    # POI / route / user tag settings
│   ├── map.pgm              # map image
│   └── map.yaml             # map metadata
└── site_b/
    ├── mapoi_config.yaml
    ├── map.pgm
    └── map.yaml
```

For the `mapoi_config.yaml` format, see the [`mapoi_server` README](../mapoi_server/README.md).

## 2. Adding mapoi to your launch file

Add the mapoi nodes to `your_robot_launch.yaml`.

```yaml
# mapoi_server: manages map/POI information and provides the mapoi services
- node:
    pkg: mapoi_server
    exec: mapoi_server
    param:
      - {name: maps_path, value: "$(find-pkg-share your_package)/maps"}
      - {name: map_name, value: "site_a"}
      - {name: config_file, value: "mapoi_config.yaml"}

# mapoi_nav2_bridge: bridges POI-name-based goals to Nav2 (autonomous navigation + POI radius event detection)
- node:
    pkg: mapoi_server
    exec: mapoi_nav2_bridge

# mapoi_amcl_localization_bridge: feeds the initial pose to AMCL via /initialpose
- node:
    pkg: mapoi_server
    exec: mapoi_amcl_localization_bridge

# mapoi_rviz2_publisher: publishes POI markers for RViz2
- node:
    pkg: mapoi_server
    exec: mapoi_rviz2_publisher

# Mapoi Web UI (optional): map display, POI editing, and navigation operation from a browser
- node:
    pkg: mapoi_webui
    exec: mapoi_webui_node.py
    param:
      - {name: maps_path, value: "$(find-pkg-share your_package)/maps"}
      - {name: map_name, value: "site_a"}
      - {name: web_port, value: 8765}
```

For an example of launching `mapoi_bringup.launch.yaml` directly from the CLI (including the argument semantics), see [`mapoi_server/README.md`](../mapoi_server/README.md).

## 3. Configuring AMCL parameters

`mapoi_amcl_localization_bridge` automatically publishes the initial pose to the `/initialpose` topic, using the **first non-landmark POI in the POI list** of `mapoi_config.yaml` as the default (#144 removed the old `initial_pose` system tag and unified the semantics into yaml ordering; #209 split the AMCL adapter out of `mapoi_nav2_bridge`). To pick a POI explicitly, use the `initial_poi_name` argument of the `mapoi/select_map` service. To avoid managing the initial pose twice with AMCL's `set_initial_pose` (Nav2-native self-init), disable it on the AMCL side, and set `first_map_only_` to `False` to enable map switching.

> **Note (distro difference)**: on Humble the AMCL parameter is named `first_map_only_` (with a trailing underscore); on Jazzy it is `first_map_only`. See `mapoi_turtlebot3_example/param/humble/burger.yaml` and `mapoi_turtlebot3_example/param/jazzy/burger.yaml` for working examples of each.

```yaml
amcl:
  ros__parameters:
    # The initial pose is delivered by mapoi_amcl_localization_bridge via the /initialpose topic
    # AMCL-native self-init is disabled to keep POIs the single source of truth
    set_initial_pose: False
    # Enable map switching (Humble spelling; on Jazzy the parameter is first_map_only)
    first_map_only_: False
```

On the `mapoi_config.yaml` side, plain POI definitions are all you need (just place the POI to use as the default initial pose at the **top** of the yaml):

```yaml
poi:
  # POI names are generic examples showing the structure (independent of the actual turtlebot3 demo POI names)
  - name: entrance        # this POI becomes the default initial pose (first in the POI list)
    pose: {x: -2.0, y: -0.5, yaw: 0.0}
    tags: [waypoint]
  - name: room_a
    pose: {x: 1.0, y: 0.5, yaw: 1.57}
    tags: [waypoint]
```

For supporting localization packages that use a different topic name or message type, see the ["Requirements for a compatible localization package"](../mapoi_server/README.md#requirements-for-a-compatible-localization-package) section of `mapoi_server/README.md`.

## 4. Using POI radius events (optional)

While driving a route, entering a route-registered POI publishes three event types — `EVENT_ENTER` / `EVENT_PAUSED` (pause tag only) / `EVENT_EXIT` — to the `mapoi/events` topic. Events do not fire outside route driving (IDLE / GOAL mode, standalone `NavigateToPose` goals, or manual teleoperation).

```sh
# Check the events
ros2 topic echo /mapoi/events
```

For the detailed spec, see the ["POI radius event detection"](../mapoi_server/README.md#poi-radius-event-detection-poievent) section of the `mapoi_server` README.
