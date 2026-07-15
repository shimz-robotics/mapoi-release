# Migration guides

> Japanese version: [README.ja.md](./README.ja.md)

Step-by-step upgrade guides for releases with breaking changes. Each guide lists the old → new mappings and, where applicable, `grep` / `sed` commands to audit your own code. See [`CHANGELOG.rst`](../../CHANGELOG.rst) for the full change history.

| Upgrade | Guide | Main breaking changes |
| --- | --- | --- |
| v0.4.x → v0.5.0 | [v0.5.0.md](./v0.5.0.md) | Service namespace `mapoi/` prefix, `GetRoutePois` success field, removal of `PointOfInterest.id`, `SelectMap` field rename, REST API URL reorg, `PoiEvent` simplification |
| v0.3.x → v0.4.0 | [v0.4.0.md](./v0.4.0.md) | `mapoi_nav_server` → `mapoi_nav2_bridge` rename, AMCL bridge split, system tag hardcoding |
| v0.2.x → v0.3.0 | [v0.3.0.md](./v0.3.0.md) | Topic namespace reorganized under `/mapoi/...` |

When skipping releases (e.g. v0.3.x → v0.5.0), apply the guides in order from oldest to newest.
