// @vitest-environment jsdom
//
// vitest unit tests for the minimal toast helper in mapoi_webui/web/js/toast.js
// (#354: command_rejected イベント通知の表示)。
//
// テスト方針: `mapoi/nav/command_rejected` SSE event → toast 表示という配線の核心は
// app.js 側 (EventSource ハンドラ) だが、そこは DOM/EventSource 依存で厚い統合テストに
// なるため、ここでは toast.js の pure な表示契約 (表示される / 複数積み上がる / 自動
// 消滅する / doc 不在で落ちない) だけを pin する。
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import * as toast from '../../web/js/toast.js';

const { showToast, ensureContainer } = toast;

beforeEach(() => {
  document.body.innerHTML = '';
});

afterEach(() => {
  vi.useRealTimers();
});

describe('ensureContainer', () => {
  it('#toast-container が無ければ作って body に追加する', () => {
    expect(document.getElementById('toast-container')).toBeNull();
    const container = ensureContainer(document);
    expect(container.id).toBe('toast-container');
    expect(document.body.contains(container)).toBe(true);
  });

  it('既にあれば使い回す (idempotent)', () => {
    const first = ensureContainer(document);
    const second = ensureContainer(document);
    expect(second).toBe(first);
    expect(document.querySelectorAll('#toast-container').length).toBe(1);
  });

  it('doc=null は no-op で null を返す', () => {
    expect(ensureContainer(null)).toBeNull();
  });
});

describe('showToast', () => {
  it('メッセージを持つ toast 要素を追加する', () => {
    const el = showToast(document, 'Command rejected: kitchen');
    expect(el.textContent).toBe('Command rejected: kitchen');
    expect(el.className).toBe('toast');
    expect(document.getElementById('toast-container').contains(el)).toBe(true);
  });

  it('複数回呼ぶと積み重なる (置き換わらない)', () => {
    showToast(document, 'first');
    showToast(document, 'second');
    const container = document.getElementById('toast-container');
    expect(container.children.length).toBe(2);
    expect(container.children[0].textContent).toBe('first');
    expect(container.children[1].textContent).toBe('second');
  });

  it('durationMs 経過後に自動で消える (既定 5000ms)', () => {
    vi.useFakeTimers();
    const el = showToast(document, 'will vanish');
    const container = document.getElementById('toast-container');
    expect(container.contains(el)).toBe(true);

    vi.advanceTimersByTime(4999);
    expect(container.contains(el)).toBe(true);

    vi.advanceTimersByTime(1);
    expect(container.contains(el)).toBe(false);
  });

  it('durationMs を指定するとその時間で消える', () => {
    vi.useFakeTimers();
    const el = showToast(document, 'quick', 1000);
    const container = document.getElementById('toast-container');

    vi.advanceTimersByTime(1000);
    expect(container.contains(el)).toBe(false);
  });

  it('doc=null は no-op で null を返す (throw しない)', () => {
    expect(() => showToast(null, 'x')).not.toThrow();
    expect(showToast(null, 'x')).toBeNull();
  });
});
