# mapoi (metapackage)

Metapackage that installs the core packages — [mapoi_server](../mapoi_server/), [mapoi_interfaces](../mapoi_interfaces/), [mapoi_rviz_plugins](../mapoi_rviz_plugins/), and [mapoi_webui](../mapoi_webui/) — as a single unit.

[mapoi_turtlebot3_example](../mapoi_turtlebot3_example/) is intentionally **not** included (the same policy as navigation2, which excludes the demo-oriented `nav2_bringup` from its metapackage): including it would pull the TurtleBot3 / Gazebo simulator stack into real-robot and production installs. To try the demo, install `mapoi_turtlebot3_example` directly — it pulls in the full mapoi core through its own dependencies.

See the [root README](../README.md) for the overview, quickstart, and documentation index.
