# operator map switch や reload による initial pose 候補通知 (#149 round 8 で字面付き型化)。
# mapoi_server が唯一の publisher (#211)。mapoi_nav2_bridge / WebUI / RViz panel は
# request_initial_pose service 経由で mapoi_server に publish を依頼する。
# - map_name : 通知対象の map (= MapoiServer::map_name_)。情報用フィールド (世代識別の素材)。
#              現状の subscriber 実装では検証に使っていない。#299 以降、非空の map_name は
#              publish 時点の mapoi_server の現在 map と一致することが保証される
#              (request_initial_pose service が不一致を reject。server 内部の publish も
#              常に現在 map を入れる)。map 不明の requester が service に空 map_name を
#              渡した場合のみ空文字がありうる。
# - poi_name : 採用する POI 名。空文字は「採用候補なし」を意味し、subscriber は無視する
#              (default の POI list 先頭採用は select_map response で
#              既に解決済。空文字を送っても何も起こらない)。
# QoS: transient_local (depth=1) で後起動 subscriber も latched 値を受信できる。
# 単一 writer 集約 (#211) により clear が last-write-wins で効く。
string map_name
string poi_name
