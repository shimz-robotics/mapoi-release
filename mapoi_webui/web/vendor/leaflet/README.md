# Leaflet (vendored)

Leaflet をオフライン環境 (ロボット LAN) でも動作させるため、npm 公式 tarball から
必要ファイルのみを抜き出して同梱している。

## バージョン・取得情報

| 項目 | 値 |
|---|---|
| バージョン | 1.9.4 |
| 取得元 | https://registry.npmjs.org/leaflet/-/leaflet-1.9.4.tgz |
| tarball sha256 | 84c65a256e50657896f54c33bd857b6849ebe94c817803be818bf32a3dde0b77 |
| 取得日 | 2026-07-12 |
| ライセンス | BSD-2-Clause (LICENSE ファイル同梱) |

## 同梱ファイル

```
leaflet.css
leaflet.js
leaflet.js.map
images/
  layers-2x.png
  layers.png
  marker-icon-2x.png
  marker-icon.png
  marker-shadow.png
LICENSE
README.md  (このファイル)
```

`leaflet-src.*` は開発用ビルドのため配布物最小化の方針で除外している。
`leaflet.js.map` は leaflet.js 末尾の `sourceMappingURL` 参照先として同梱する
(除外すると DevTools やモニタリングで 404 が出るため。leaflet.css に map 参照は無い)。

## 更新手順

新版リリース時は以下の手順で差し替える:

1. npm registry から新バージョンの tarball を取得する
   ```
   curl -L https://registry.npmjs.org/leaflet/-/leaflet-<VERSION>.tgz -o leaflet-<VERSION>.tgz
   sha256sum leaflet-<VERSION>.tgz   # 値をこの README に記録する
   tar xzf leaflet-<VERSION>.tgz
   ```
2. 展開された `package/dist/leaflet.js` / `package/dist/leaflet.js.map` / `package/dist/leaflet.css` /
   `package/dist/images/*.png` および `package/LICENSE` をこのディレクトリの同名ファイルに上書きコピーする
3. このファイルのバージョン・取得元・sha256・取得日を更新する
4. `web/index.html` の `<link>` / `<script>` タグはローカル相対パスのままなので変更不要
5. 検証: 公式 dist との bit 一致と必須ファイルの存在を確認する
   ```
   diff -r package/dist/leaflet.js leaflet.js && diff -r package/dist/images images
   npx vitest run vendor-leaflet-static.test.js   # mapoi_webui/tests/web/ から
   ```
