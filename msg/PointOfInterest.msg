# A point of interest (POI) with position, tolerance, and classification tags
string name                           # Human-readable name, the de facto unique key
geometry_msgs/Pose pose               # 6-DOF pose in the map frame
mapoi_interfaces/Tolerance tolerance  # Nav2 align tolerance (xy + yaw)。tolerance.xy は
                                      # POI radius (進入判定距離) としても流用される。
                                      # v0.3.0 で旧 `radius` フィールドから移行 (#87)。
string[] tags                         # Classification tags (e.g. "goal", "event")
string description                    # Free-text description
