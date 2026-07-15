// PoiEditorPanel のテーブル編集系ハンドラと Undo/Redo 基盤 (#407)。
//
// このファイルは 2 つの責務を 1 TU にまとめる:
//   (A) #397 step 9: ResetButton / TableChanged / NewButton / CopyButton /
//       DeleteButton / RowMoved / FileComboBox を poi_editor.cpp から機械的に移設。
//   (B) #407: QUndoStack ベースの Undo/Redo (行追加 New / 複製 Copy / 削除 Delete /
//       セル編集 / 行並べ替え RowMoved の 5 操作をコマンド化)。
//
// step 9 を先行分割せず本 issue と統合したのは、Undo/Redo がこの 7 slot のブロックを
// 丸ごと QUndoCommand 化で書き換えるため、分割 → 即作り直しの二度手間を避ける狙い
// (#397 step 9 の記述・#407 提案どおり)。
//
// QUndoCommand サブクラス群は TU 内 file-local (無名 namespace) とし、hpp の公開面を
// 増やさない (#407 設計)。コマンドは panel の protected メンバへ直接触らず、hpp に最小限
// 追加した「適用ヘルパー member 関数」(Apply*) 経由でテーブルを書き換える。
#include "mapoi_rviz_plugins/poi_editor.hpp"
#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"

#include <algorithm>  // std::min (ApplyInsertRow の shadow_row clamp)
#include <utility>    // std::move (コマンドへのテキスト move)
#include <vector>

#include <QBrush>  // 編集済みマークの着色/解除 (#445, RefreshCellEditMark / ClearAllEditMarks)
#include <QFileDialog>
#include <QHeaderView>  // verticalHeader()->visualIndex / moveSection (#434, ApplyInsertRow の視覚移動)
#include <QScopedValueRollback>
#include <QShortcut>
#include <QUndoCommand>
#include <QUndoStack>

// generated UI header: PoiEditorPanel::ui_ (`Ui::PoiEditorUi*`) は poi_editor.hpp では
// 前方宣言のみなので、`ui_->PoiTable` 等の完全型アクセスにはこの include が要る
// (poi_editor_save.cpp / poi_editor_validation.cpp と同じ)。
#include "ui_poi_editor.h"

namespace mapoi_rviz_plugins
{

// PoiTable column index 定数は poi_editor_helpers.hpp に集約 (#158 で導入)。
using detail::kColName;
using detail::kColPose;
using detail::kColTolerance;
using detail::kColTags;
using detail::kColDescription;
using detail::kColCount;

// ---------------------------------------------------------------------------
// QUndoCommand サブクラス群 (#407)
// ---------------------------------------------------------------------------
// いずれも TU 内 file-local (無名 namespace)。panel の protected メンバへは触らず、
// hpp に追加した適用ヘルパー (ApplyInsertRow / ApplyRemoveRow / ApplySetCell /
// ApplyMoveSection) 経由でテーブルを書き換える。ヘルパー側で applying_undo_ を立てて
// cellChanged / sectionMoved の再入を抑制し、shadow model も同期する (下記参照)。
//
// 各コマンドは undo/redo に必要な最小の値 (行 index・列・old/new テキスト・行の全 cell
// テキスト) を自前で保持する。QTableWidgetItem のポインタは Qt がテーブル操作時に破棄・
// 再生成するため保持せず、常に「行 index + テキスト」で持つ。
namespace
{

// 行全体 (kColCount 列) のテキストを保持する軽量な行スナップショット。
using RowTexts = std::vector<QString>;

// セル 1 個の編集 (#407)。TableChanged が shadow との差分から old/new を得て生成する。
// redo = new を書き込む、undo = old を書き戻す。行構造は変えないため index は不変。
class EditCellCommand : public QUndoCommand
{
public:
  EditCellCommand(PoiEditorPanel * panel, int row, int col,
                  QString old_text, QString new_text)
  : panel_(panel), row_(row), col_(col),
    old_text_(std::move(old_text)), new_text_(std::move(new_text))
  {
    setText(QObject::tr("edit cell"));
  }

  void undo() override { panel_->ApplySetCell(row_, col_, old_text_); }
  void redo() override { panel_->ApplySetCell(row_, col_, new_text_); }

private:
  PoiEditorPanel * panel_;
  int row_;
  int col_;
  QString old_text_;
  QString new_text_;
};

// 行追加 (New) / 複製 (Copy) 共通。redo = row_ に texts_ の行を挿入、undo = row_ を削除。
// New は "new_poi" 既定行、Copy は複製元行のテキストを texts_ に持つ (生成側で構築)。
//
// visual_ref_ (#434): 選択行の logical index。redo() で ApplyInsertRow に渡し、視覚順を
// 並べ替え済みのテーブルでも新規行を選択行の視覚直下へ moveSection する。視覚移動は redo 経路
// (ApplyInsertRow) に置くため、undo→redo のやり直しでも同じ視覚位置に再現される。undo は
// ApplyRemoveRow が挿入した section をそのまま除去するため、視覚移動の明示的な逆適用は不要
// (残り行の相対視覚順は removeRow で保たれる)。選択なし (New で -1) は視覚移動しない。
class InsertRowCommand : public QUndoCommand
{
public:
  InsertRowCommand(PoiEditorPanel * panel, int row, RowTexts texts, const QString & label,
                   int visual_ref)
  : panel_(panel), row_(row), texts_(std::move(texts)), visual_ref_(visual_ref)
  {
    setText(label);
  }

  void undo() override { panel_->ApplyRemoveRow(row_); }
  void redo() override { panel_->ApplyInsertRow(row_, texts_, visual_ref_); }

private:
  PoiEditorPanel * panel_;
  int row_;
  RowTexts texts_;
  int visual_ref_;
};

// 行削除 (Delete)。redo = row_ を削除、undo = row_ に texts_ を挿入 (削除前の全 cell
// テキストを保持しておき復元)。生成側で削除前に行テキストをキャプチャする。
//
// 既知制限 (PR #426 review): 行ドラッグ (visual 並び替え) 済みのテーブルで削除→undo すると、
// 内容は logical index に正しく復元されるが、visual 位置は既定 (logical と同じ) に戻る
// (QHeaderView は削除された section の visual 位置を記憶しない)。visual 位置まで復元するには
// visualIndex のキャプチャ + moveSection 逆適用が要るが、複合エッジ (並び替え+削除+undo) の
// 頻度に対して複雑化が見合わないため割り切る。保存後の UpdatePoiTable 再構築で visual =
// logical に正規化される点は従来通り。
// なお New/Copy の挿入は #434 で選択行の視覚直下へ揃えるようにしたが、これは RemoveRowCommand の
// undo (再挿入) には及ぼさない — ApplyInsertRow を visual_ref_row = -1 (既定) で呼ぶため、上記の
// 削除 undo の視覚挙動 (既定に戻る) はこれまで通り。
class RemoveRowCommand : public QUndoCommand
{
public:
  RemoveRowCommand(PoiEditorPanel * panel, int row, RowTexts texts)
  : panel_(panel), row_(row), texts_(std::move(texts))
  {
    setText(QObject::tr("delete row"));
  }

  void undo() override { panel_->ApplyInsertRow(row_, texts_); }
  void redo() override { panel_->ApplyRemoveRow(row_); }

private:
  PoiEditorPanel * panel_;
  int row_;
  RowTexts texts_;
};

// 行並べ替え (RowMoved)。verticalHeader()->moveSection の逆適用で undo する (#407 §8)。
// sectionMoved は (logicalIndex, oldVisual, newVisual) を渡すので、redo = old→new、
// undo = new→old へ moveSection する。moveSection の再入抑制も適用ヘルパー側で行う。
//
// 他コマンド (Insert/Remove) と非対称な点: New/Copy/Delete slot は「push するだけ・実操作は
// redo」設計なので QUndoStack::push が呼ぶ初回 redo() が実操作を担う。一方 RowMoved は
// sectionMoved シグナル由来 = ユーザーが既にドラッグで移動を完了した後に発火するため、
// push 時の初回 redo() で再度 moveSection(old→new) すると二重移動になる。first_redo_ で
// 初回 redo() のみ実 moveSection をスキップする (以降の redo = やり直しでは実行する)。
class MoveRowCommand : public QUndoCommand
{
public:
  MoveRowCommand(PoiEditorPanel * panel, int old_visual, int new_visual)
  : panel_(panel), old_visual_(old_visual), new_visual_(new_visual)
  {
    setText(QObject::tr("move row"));
  }

  void undo() override { panel_->ApplyMoveSection(new_visual_, old_visual_); }
  void redo() override
  {
    // 初回 redo (= push 時) は既にユーザー操作で移動済みなので実行しない (二重移動防止)。
    // dirty / 着色 / SaveButton 文言は RowMoved slot 側で付与済み。
    if (first_redo_) {
      first_redo_ = false;
      return;
    }
    panel_->ApplyMoveSection(old_visual_, new_visual_);
  }

private:
  PoiEditorPanel * panel_;
  int old_visual_;
  int new_visual_;
  bool first_redo_ = true;
};

// 指定行の全 cell テキストを取り出す (Copy の複製元・Delete の復元用スナップショット)。
RowTexts CaptureRowTexts(QTableWidget * table, int row)
{
  RowTexts texts;
  texts.reserve(kColCount);
  for (int col = 0; col < kColCount; ++col) {
    auto * item = table->item(row, col);
    texts.push_back(item ? item->text() : QString());
  }
  return texts;
}

}  // namespace

// ---------------------------------------------------------------------------
// Undo/Redo 基盤の初期化・適用ヘルパー (#407)
// ---------------------------------------------------------------------------

void PoiEditorPanel::SetupUndoRedo()
{
  // QUndoStack は panel が親 (QObject 所有権) なので明示 delete 不要。
  undo_stack_ = new QUndoStack(this);

  // ショートカット (#407 §7)。context は WidgetWithChildrenShortcut にして、パネル
  // (と子ウィジェット) に focus がある時だけ有効化する。RViz 全体のキーマップと衝突させない
  // (issue 要件)。QKeySequence::Undo = Ctrl+Z、Redo = Ctrl+Shift+Z (プラットフォーム標準)。
  auto * undo_shortcut = new QShortcut(QKeySequence::Undo, this);
  undo_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(undo_shortcut, &QShortcut::activated, undo_stack_, &QUndoStack::undo);

  auto * redo_shortcut = new QShortcut(QKeySequence::Redo, this);
  redo_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(redo_shortcut, &QShortcut::activated, undo_stack_, &QUndoStack::redo);

  // Undo/Redo ボタン (#435)。ショートカットは WidgetWithChildrenShortcut のためパネル内に
  // focus が無いと無反応で、キーボード専用では発見しづらい (#407 の PR #426 で「実装されて
  // いない」と誤認された経緯)。ボタンは focus 状態に関係なくクリックで動作する。初期状態は
  // .ui 側で disabled にしてあり、canUndoChanged/canRedoChanged で有効/無効を連動させる
  // (履歴が空の間は押せない)。
  connect(undo_stack_, &QUndoStack::canUndoChanged, ui_->UndoButton, &QPushButton::setEnabled);
  connect(undo_stack_, &QUndoStack::canRedoChanged, ui_->RedoButton, &QPushButton::setEnabled);
  connect(ui_->UndoButton, &QPushButton::clicked, undo_stack_, &QUndoStack::undo);
  connect(ui_->RedoButton, &QPushButton::clicked, undo_stack_, &QUndoStack::redo);

  // dirty 連携 (#407 §6, #399)。undo で編集前 (clean) に戻ったら未保存ガードを解除する。
  // WebUI の「undo で clean に戻るとガード解除」(beforeunload.e2e.js の
  // "undo back to clean removes the guard again") と同じ意味論。clean=false へ移る
  // (= 編集が入る) 側は既存の table_dirty_ 手動 set が担うため、ここでは clean へ戻った
  // 時だけ dirty=false にする (既存 dirty set は不変のまま連携させる)。
  connect(undo_stack_, &QUndoStack::cleanChanged, this, [this](bool clean) {
    if (clean) {
      // clean へ戻った = テーブルは基準状態 (#445)。編集済みマークを全解除する。undo で
      // 戻った場合、行並べ替え (RowMoved) 由来の行着色はテキスト比較 (RefreshCellEditMark)
      // では検出できないため、ここでまとめて掃除する。保存成功・全再構築の stack clear
      // 経由でも走るが無害 (どちらも着色なしへ向かう状態)。
      ClearAllEditMarks();
      table_dirty_ = false;
    }
  });
}

void PoiEditorPanel::RebuildShadowModel()
{
  // shadow model の (再) 構築 (#407 §3)。テーブル内容のミラーを持ち、cellChanged 時に
  // shadow との差分から old 値を得る (QTableWidget の cellChanged は旧値を渡さないため)。
  // UpdatePoiTable / TagFilterChanged の全再構築時に取り直す (呼び出しは各所から)。
  const int rows = ui_->PoiTable->rowCount();
  const int cols = ui_->PoiTable->columnCount();
  shadow_.assign(rows, std::vector<QString>(cols));
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      auto * item = ui_->PoiTable->item(r, c);
      shadow_[r][c] = item ? item->text() : QString();
    }
  }
  // 着色基準 (#445) も同時に取り直す。全再構築直後のテーブルは clean 状態 (無着色) であり、
  // ここを基準に「テキストが基準と異なるセルだけ緑」を判定する。
  clean_texts_ = shadow_;
}

void PoiEditorPanel::RefreshCellEditMark(int row, int col)
{
  // 「緑 = clean 基準とテキストが異なるセル」(#445)。基準に一致したら既定背景へ戻す
  // (QBrush() は「brush 未設定」= スタイル既定色に委ねる)。基準が無い範囲 (通常は無いが
  // 防御的に) は従来どおり緑側に倒す。setBackground は同値なら dataChanged を発火しない
  // (Qt 5.12+ の setData 同値 early-return) ため冪等。
  auto * item = ui_->PoiTable->item(row, col);
  if (!item) {
    return;
  }
  bool matches_clean = false;
  if (row >= 0 && row < static_cast<int>(clean_texts_.size()) &&
      col >= 0 && col < static_cast<int>(clean_texts_[row].size())) {
    matches_clean = (clean_texts_[row][col] == item->text());
  }
  item->setBackground(matches_clean ? QBrush() : QBrush(Qt::green));
}

void PoiEditorPanel::ClearAllEditMarks()
{
  // setBackground は cellChanged (dataChanged) を発火し得るため、applying_undo_ を立てて
  // TableChanged の再処理 (dirty 再 set・SaveButton 文言更新・コマンド化) を抑制する。
  const QScopedValueRollback<bool> guard(applying_undo_, true);
  const int rows = ui_->PoiTable->rowCount();
  const int cols = ui_->PoiTable->columnCount();
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      if (auto * item = ui_->PoiTable->item(r, c)) {
        item->setBackground(QBrush());
      }
    }
  }
}

void PoiEditorPanel::ApplySetCell(int row, int col, const QString & text)
{
  // コマンドの undo()/redo() からのセル書き換え。applying_undo_ を立てて cellChanged→
  // TableChanged の再入 (dirty 再 set・二重コマンド化) を抑制する (#407 §4)。
  // 既存の is_table_color_ ガード (再構築中の cellChanged を弾く) とは目的が異なるので
  // 別フラグで共存させる (is_table_color_ は着色/dirty の可否、applying_undo_ は
  // コマンド適用中の TableChanged 全処理スキップ)。
  {
    // QScopedValueRollback で例外・早期 return でもフラグが必ず復元されるようにする
    // (true のまま残ると以降の編集が全て握りつぶされるため、PR #426 review)。
    const QScopedValueRollback<bool> guard(applying_undo_, true);
    if (row >= 0 && row < ui_->PoiTable->rowCount()) {
      // 既存 item があれば setText で更新する (setItem による delete/再生成を避ける)。
      // push 時の初回 redo() は cellChanged ハンドラ (TableChanged) の呼び出し中に走るため、
      // シグナルを発火させた item 自体を破棄すると再入安全性が脆くなる。setText なら
      // item 実体を保ったまま値だけ更新でき、同値上書き (冪等 redo) も無害。
      if (auto * existing = ui_->PoiTable->item(row, col)) {
        existing->setText(text);
      } else {
        ui_->PoiTable->setItem(row, col, new QTableWidgetItem(text));
      }
      // 編集済みの視覚マーク (#445): clean 基準との比較で着色/解除する。undo で値が基準へ
      // 戻ったセルは緑が解除され、redo で再び基準から離れれば緑に戻る。
      RefreshCellEditMark(row, col);
      // shadow を書き換え後の値に同期する (次の cellChanged 差分計算の基準を更新)。
      if (row < static_cast<int>(shadow_.size()) &&
          col < static_cast<int>(shadow_[row].size())) {
        shadow_[row][col] = text;
      }
    }
  }
  // 名前セルの undo/redo は TableChanged と同じくその行のフィルタを再評価する (#405 整合)。
  // undo で名前を戻したら隠れていた行が再表示される / redo で非一致になれば隠れる、を
  // 通常編集と揃える。applying_undo_ を戻した後に呼ぶ (setRowHidden は cellChanged 非発火)。
  if (col == kColName) {
    ApplyNameFilterToRow(row);
  }
  // dirty / SaveButton 表示はコマンド適用でも通常編集と同じく更新する。
  // 順序保証 (#407 §6): QUndoStack::undo() は「command->undo() 実行 → index 更新 →
  // cleanChanged emit」の順で走る。ここ (command->undo() 内) で table_dirty_=true にしても、
  // その直後の cleanChanged(true) が table_dirty_=false へ上書きするため、最後の 1 手を
  // undo して clean に戻った時は最終的に dirty=false になる (WebUI の「undo で clean に
  // 戻るとガード解除」と一致)。まだ手が残る undo なら clean にならず dirty=true が残る。
  table_dirty_ = true;
  ui_->SaveButton->setText("save");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
  UpdatePoiCount();
}

void PoiEditorPanel::ApplyInsertRow(int row, const std::vector<QString> & texts, int visual_ref_row)
{
  // 行挿入 (New/Copy の redo、Delete の undo)。insertRow は cellChanged を確実には
  // 発火しないが、setItem 経由の cellChanged が飛ぶ可能性があるため applying_undo_ で
  // 抑制する (二重コマンド化・dirty の重複計上を防ぐ)。
  {
    const QScopedValueRollback<bool> guard(applying_undo_, true);  // 例外安全 (PR #426 review)
    ui_->PoiTable->insertRow(row);
    for (int col = 0; col < kColCount && col < static_cast<int>(texts.size()); ++col) {
      ui_->PoiTable->setItem(row, col, new QTableWidgetItem(texts[col]));
    }
    // shadow に行を挿入して以降の差分計算の基準を保つ。着色基準 (#445) にも同じ行を挿入して
    // index を揃える。挿入行の着色基準 = 挿入時テキストとし、従来の「挿入行は無着色」挙動を
    // 保ちつつ、以後のその行の編集・undo を比較で判定できるようにする。
    std::vector<QString> row_texts(texts.begin(),
      texts.begin() + std::min(texts.size(), static_cast<size_t>(kColCount)));
    row_texts.resize(kColCount);
    if (row >= 0 && row <= static_cast<int>(shadow_.size())) {
      shadow_.insert(shadow_.begin() + row, row_texts);
    }
    if (row >= 0 && row <= static_cast<int>(clean_texts_.size())) {
      clean_texts_.insert(clean_texts_.begin() + row, std::move(row_texts));
    }
    // #434: New/Copy 経路 (visual_ref_row >= 0) は、視覚順を並べ替え済みのテーブルでも
    // 新規行が選択行の視覚直下に来るよう moveSection する。Qt は挿入 section の視覚位置を
    // 選択行と無関係に決めるため、挿入後の実 visual index を読んで移動先を計算する。
    // visual_ref_row (= 選択行 logical) は row = 選択行+1 なので insertRow で index が
    // ずれず、visualIndex(visual_ref_row) で選択行の現在 visual を得られる。moveSection は
    // sectionMoved を発火するが applying_undo_ 中なので RowMoved slot は早期 return し
    // 二重コマンド化しない (この guard ブロック内で実行するのが要件)。
    // visual_ref_row < row も条件で強制する (ヘッダ記載の事前条件)。満たさない呼び出しは
    // 挿入で参照行の logical がずれ誤った行の直下へ移動し得るため、視覚移動なしへ縮退させる。
    if (visual_ref_row >= 0 && visual_ref_row < row) {
      auto * vheader = ui_->PoiTable->verticalHeader();
      const int inserted_visual = vheader->visualIndex(row);
      const int ref_visual = vheader->visualIndex(visual_ref_row);
      if (inserted_visual >= 0 && ref_visual >= 0) {
        const int target = detail::insert_move_target_visual(inserted_visual, ref_visual);
        if (inserted_visual != target) {
          vheader->moveSection(inserted_visual, target);
        }
      }
    }
  }
  table_dirty_ = true;
  // 挿入行は名前フィルタで再評価しない (#405 の「New/Copy の新規行はフィルタ非一致でも
  // デフォルト可視」意図を維持)。Delete の undo による行復元でも復元行を可視で戻すことで
  // ユーザーが編集を継続できる (元の New/Copy がフィルタ処理を持たなかったのと同じ挙動)。
  // insertRow は非表示状態を継承しないため既定で可視になる。
  UpdatePoiCount();
}

void PoiEditorPanel::ApplyRemoveRow(int row)
{
  // 行削除 (Delete の redo、New/Copy の undo)。removeRow は cellChanged を発火しないが、
  // 対称性のため applying_undo_ で括る。shadow からも同じ行を落とす。
  {
    const QScopedValueRollback<bool> guard(applying_undo_, true);  // 例外安全 (PR #426 review)
    if (row >= 0 && row < ui_->PoiTable->rowCount()) {
      ui_->PoiTable->removeRow(row);
    }
    if (row >= 0 && row < static_cast<int>(shadow_.size())) {
      shadow_.erase(shadow_.begin() + row);
    }
    // 着色基準 (#445) からも同じ行を落として index を揃える。
    if (row >= 0 && row < static_cast<int>(clean_texts_.size())) {
      clean_texts_.erase(clean_texts_.begin() + row);
    }
  }
  table_dirty_ = true;
  ui_->SaveButton->setText("save");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
  UpdatePoiCount();
}

void PoiEditorPanel::ApplyMoveSection(int from_visual, int to_visual)
{
  // 行並べ替えの適用 (RowMoved のコマンド化、#407 §8)。verticalHeader()->moveSection は
  // sectionMoved を発火するため、applying_undo_ で RowMoved slot の再入 (二重コマンド化)
  // を抑制する。moveSection は visual index 基準 (logical は不変) なので shadow は
  // 触らない (shadow は logical row の内容ミラーであり visual 並びに依存しない — #405 の
  // 名前フィルタが logical 基準なのと同じ扱い)。
  {
    const QScopedValueRollback<bool> guard(applying_undo_, true);  // 例外安全 (PR #426 review)
    ui_->PoiTable->verticalHeader()->moveSection(from_visual, to_visual);
  }
  table_dirty_ = true;
  ui_->SaveButton->setText("save");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
}

// ---------------------------------------------------------------------------
// テーブル編集系 slot (#397 step 9 で poi_editor.cpp から移設 + #407 でコマンド化)
// ---------------------------------------------------------------------------

void PoiEditorPanel::ResetButton()
{
  PoiEditorPanel::UpdatePoiTable();
}

void PoiEditorPanel::TableChanged(int row, int column)
{
  // コマンドの undo()/redo() 由来の setItem では applying_undo_ が立っている。この時は
  // dirty 再 set・二重コマンド化・着色を全てスキップする (適用ヘルパー側が dirty/着色/
  // SaveButton を一括更新するため、ここで重ねない — #407 §4)。
  if (applying_undo_) {
    return;
  }

  if(is_table_color_){
    // 編集済みの視覚マーク (#445): 無条件の緑でなく clean 基準との比較で着色/解除する。
    // 手入力で元の値に戻した場合も緑が解除される (undo と意味を揃える)。
    RefreshCellEditMark(row, column);
    // ユーザー編集の dirty マーク (#399)。UpdatePoiTable / TagFilterChanged の再構築中は
    // is_table_color_=false で setItem 由来の cellChanged が飛ぶため、既存の green 着色ガードに
    // 相乗りしてユーザー編集だけを拾う (再構築由来の cellChanged では dirty を立てない)。
    table_dirty_ = true;

    // セル編集のコマンド化 (#407 §3)。cellChanged は旧値を渡さないため shadow model との
    // 差分から old 値を得てコマンドを push し、shadow を新値へ更新する。値が変わっていない
    // (old == new) 場合はコマンドを積まない (フィルタ再評価等で発火する空 cellChanged を弾く)。
    auto * item = ui_->PoiTable->item(row, column);
    const QString new_text = item ? item->text() : QString();
    QString old_text;
    if (row < static_cast<int>(shadow_.size()) &&
        column < static_cast<int>(shadow_[row].size())) {
      old_text = shadow_[row][column];
    }
    // shadow_ 未構築 (行/列がまだ無い) 状態での書き込みは範囲外アクセスになるため、
    // ApplySetCell (:258-260) と同型の境界チェックで弾く (#432)。
    if (undo_stack_ && old_text != new_text &&
        row < static_cast<int>(shadow_.size()) &&
        column < static_cast<int>(shadow_[row].size())) {
      // shadow を先に更新してから push する。EditCellCommand の redo() は生成直後に
      // QUndoStack::push が呼ぶが、その redo() は ApplySetCell(new_text) で「既に new_text
      // が入っている」テーブルへ同じ値を上書きするだけ (冪等) なので副作用は無い。
      shadow_[row][column] = new_text;
      undo_stack_->push(new EditCellCommand(this, row, column, old_text, new_text));
    }

    if (column == kColName) {
      // 名前セルの編集確定時はその行だけ名前フィルタを再評価する (#405、PR #420 review)。
      // 一致しなくなった行は編集確定と同時に隠れる (絞り込み表示の一貫性を優先)。
      ApplyNameFilterToRow(row);
    }
  }
  ui_->SaveButton->setText("save");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
  UpdatePoiCount();
}

void PoiEditorPanel::NewButton()
{
  int current_row = ui_->PoiTable->currentRow();
  int new_row = current_row + 1;
  // column 構造 (#158): name / pose / tolerance "xy m, yaw rad" / tags / description
  RowTexts texts = {
    QStringLiteral("new_poi"),
    QStringLiteral("0.0, 0.0, 0.0"),
    QStringLiteral("0.5, 0.7854"),  // ≒ π/4 rad
    QString(),
    QString(),
  };
  // コマンド化 (#407)。redo() = ApplyInsertRow が実際の挿入・dirty set・shadow 更新を行う。
  // insertRow / setItem は cellChanged を確実には発火しないため dirty はヘルパー側で明示 set (#399)。
  // current_row (#434): 選択行の logical index を渡し、視覚順並べ替え済みでも新規行を選択行の
  // 視覚直下へ揃える。選択なし (current_row == -1) は視覚移動せず従来挙動を維持する。
  if (undo_stack_) {
    undo_stack_->push(new InsertRowCommand(this, new_row, std::move(texts),
      QObject::tr("add row"), current_row));
  }
}

void PoiEditorPanel::CopyButton()
{
  int current_row = ui_->PoiTable->currentRow();
  if (current_row < 0) return;
  int new_row = current_row + 1;
  // 複製元行の全 cell テキストを持ってコマンド化 (#407)。redo() = ApplyInsertRow。
  // current_row (#434): 複製元 (選択行) の logical index を渡し、複製行を選択行の視覚直下へ
  // 揃える。ここでは current_row >= 0 が保証済み (上のガード) なので必ず視覚移動する。
  RowTexts texts = CaptureRowTexts(ui_->PoiTable, current_row);
  if (undo_stack_) {
    undo_stack_->push(new InsertRowCommand(this, new_row, std::move(texts),
      QObject::tr("copy row"), current_row));
  }
}

void PoiEditorPanel::DeleteButton()
{
  int current_row = ui_->PoiTable->currentRow();
  // 選択行なし (-1) の早期 return を追加 (#407)。元は removeRow(-1) が Qt 側で no-op に
  // なるだけだったが、コマンド化すると空の RemoveRowCommand が積まれて dirty が立つため
  // 明示的に弾く (元挙動「選択なしなら何も起きない」を維持。CopyButton と同じガード)。
  if (current_row < 0) return;
  // 削除前に行の全 cell テキストをキャプチャしてコマンド化 (#407)。undo() でこのテキストを
  // 同 index へ復元する。removeRow は cellChanged を発火しないため dirty はヘルパー側で明示 set (#399)。
  RowTexts texts = CaptureRowTexts(ui_->PoiTable, current_row);
  if (undo_stack_) {
    undo_stack_->push(new RemoveRowCommand(this, current_row, std::move(texts)));
  }
}

void PoiEditorPanel::RowMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex){
  // コマンドの undo()/redo() 由来の moveSection では applying_undo_ が立っているため
  // 二重コマンド化を抑制する (#407 §4/§8)。
  if (applying_undo_) {
    return;
  }
  // ユーザー操作由来の並べ替え。逆適用できるよう old/new visual index でコマンド化する。
  // undo() = new→old、redo() (やり直し) = old→new へ moveSection。push 時の初回 redo() は
  // MoveRowCommand::first_redo_ で実 moveSection をスキップする (既にドラッグで移動済み)。
  if (undo_stack_) {
    undo_stack_->push(new MoveRowCommand(this, oldVisualIndex, newVisualIndex));
  }
  // 着色・dirty・SaveButton 文言はこの slot で付与する (初回 redo が実操作を伴わないため
  // ApplyMoveSection には委ねない)。着色は従来通り logicalIndex 基準 — item() は logical
  // 行アクセスなので、visual index を渡すと並び替え後は別の行を着色してしまう。
  int numCols = ui_->PoiTable->columnCount();
  for (int col = 0; col < numCols; col++){
    if (auto * item = ui_->PoiTable->item(logicalIndex, col)) {
      item->setBackground(Qt::green);
    }
  }
  // sectionMoved は cellChanged を発火しないため dirty を明示 set (#399)。
  table_dirty_ = true;
  ui_->SaveButton->setText("save");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
}

void PoiEditorPanel::FileComboBox()
{
  int last = ui_->FileComboBox->count() - 1;
  if(ui_->FileComboBox->currentIndex() == last){
    QString filename = QFileDialog::getOpenFileName(
        this, tr("Select a poi_file"), QString::fromStdString(config_path_), tr("YAML files(*.yaml)"));
    if(filename == ""){
      ui_->FileComboBox->setItemText(last, "the other");
      ui_->FileComboBox->setCurrentIndex(0);
    }else{
      ui_->FileComboBox->setItemText(last, filename);
    }
  } else{
    int last2 = ui_->FileComboBox->count() - 1;
    ui_->FileComboBox->setItemText(last2, "the other");
  }
  ui_->SaveButton->setText("save");
}

}  // namespace mapoi_rviz_plugins
