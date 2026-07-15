# Migration ガイド

> English version (primary): [README.md](./README.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

破壊的変更を含むリリースの段階的な移行手順集です。各ガイドには新旧対応表と、必要に応じて自コードを点検する `grep` / `sed` コマンドが載っています。変更履歴の全量は [`CHANGELOG.rst`](../../CHANGELOG.rst) を参照してください。

| 対象 | ガイド | 主な破壊的変更 |
| --- | --- | --- |
| v0.4.x → v0.5.0 | [v0.5.0.ja.md](./v0.5.0.ja.md) | service 名前空間 `mapoi/` prefix 化、`GetRoutePois` success フィールド、`PointOfInterest.id` 削除、`SelectMap` フィールド rename、REST API URL 再編、`PoiEvent` 簡素化 |
| v0.3.x → v0.4.0 | [v0.4.0.ja.md](./v0.4.0.ja.md) | `mapoi_nav_server` → `mapoi_nav2_bridge` rename、AMCL bridge 分離、system tag のハードコード化 |
| v0.2.x → v0.3.0 | [v0.3.0.ja.md](./v0.3.0.ja.md) | topic 名前空間の `/mapoi/...` 再編 |

リリースをまたいで更新する場合 (例: v0.3.x → v0.5.0) は、古い順にガイドを適用してください。
