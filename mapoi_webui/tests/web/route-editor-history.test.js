// @vitest-environment jsdom
//
// RouteEditor waypoint UX regression tests (#303/#304/#305).
//
// RouteEditor is DOM-heavy, so most tests use a thin Object.create harness and
// pin the state transitions that are easy to regress: edit-form history,
// arbitrary waypoint reordering, and waypoint focus callbacks.
import { beforeEach, describe, expect, it, vi } from 'vitest';
import * as history from '../../web/js/poi-history.js';
import RouteEditor from '../../web/js/route-editor.js';

function makeEditor() {
  const editor = Object.create(RouteEditor.prototype);
  editor.editingIndex = 0;
  editor.inputRouteName = { value: 'route-a' };
  editor.editingWaypoints = ['A', 'B', 'C'];
  editor.editingLandmarks = ['L1'];
  editor.undoStack = [];
  editor.redoStack = [];
  editor.HISTORY_CAP = 50;
  editor.dragWaypointIndex = -1;
  editor.selectedWaypointIndex = -1;
  editor.btnUndo = { disabled: true };
  editor.btnRedo = { disabled: true };
  editor.waypointListEl = { querySelectorAll: () => [] };
  editor.renderWaypointList = vi.fn();
  editor.renderLandmarkList = vi.fn();
  editor.onEditingChange = vi.fn();
  return editor;
}

beforeEach(() => {
  globalThis.MapoiPoiHistory = history;
});

describe('RouteEditor edit-form history', () => {
  it('undoes and redoes arbitrary waypoint reordering', () => {
    const editor = makeEditor();

    editor.moveWaypointTo(0, 2);

    expect(editor.editingWaypoints).toEqual(['B', 'C', 'A']);
    expect(editor.btnUndo.disabled).toBe(false);
    expect(editor.btnRedo.disabled).toBe(true);

    editor.undo();
    expect(editor.editingWaypoints).toEqual(['A', 'B', 'C']);
    expect(editor.btnUndo.disabled).toBe(true);
    expect(editor.btnRedo.disabled).toBe(false);

    editor.redo();
    expect(editor.editingWaypoints).toEqual(['B', 'C', 'A']);
    expect(editor.btnUndo.disabled).toBe(false);
    expect(editor.btnRedo.disabled).toBe(true);
  });

  it('clears the redo future when a new waypoint edit happens after undo', () => {
    const editor = makeEditor();
    editor.onWaypointFocus = vi.fn();

    editor.removeWaypoint(1);
    editor.undo();
    expect(editor.redoStack).toHaveLength(1);

    editor.addWaypointByName('D');

    expect(editor.editingWaypoints).toEqual(['A', 'B', 'C', 'D']);
    expect(editor.selectedWaypointIndex).toBe(3);
    expect(editor.onWaypointFocus).toHaveBeenCalledWith('D');
    expect(editor.redoStack).toEqual([]);
    expect(editor.btnRedo.disabled).toBe(true);
  });

  it('inserts a waypoint after the selected waypoint', () => {
    const editor = makeEditor();
    editor.selectedWaypointIndex = 0;
    editor.onWaypointFocus = vi.fn();

    editor.insertWaypointAfterSelected('D');

    expect(editor.editingWaypoints).toEqual(['A', 'D', 'B', 'C']);
    expect(editor.selectedWaypointIndex).toBe(1);
    expect(editor.onWaypointFocus).toHaveBeenCalledWith('D');
  });

  it('includes landmark add/remove in the same edit-form history', () => {
    const editor = makeEditor();

    editor.removeLandmark(0);
    expect(editor.editingLandmarks).toEqual([]);

    editor.undo();
    expect(editor.editingLandmarks).toEqual(['L1']);

    editor.redo();
    expect(editor.editingLandmarks).toEqual([]);
  });

  it('does not mutate history or redraw for invalid/no-op reorders', () => {
    const editor = makeEditor();

    editor.moveWaypointTo(1, 1);
    editor.moveWaypointTo(-1, 1);
    editor.moveWaypointTo(1, 99);

    expect(editor.editingWaypoints).toEqual(['A', 'B', 'C']);
    expect(editor.undoStack).toEqual([]);
    expect(editor.renderWaypointList).not.toHaveBeenCalled();
  });

  it('keeps the selected waypoint attached to the moved item', () => {
    const editor = makeEditor();
    editor.selectedWaypointIndex = 1;

    editor.moveWaypointTo(1, 0);

    expect(editor.editingWaypoints).toEqual(['B', 'A', 'C']);
    expect(editor.selectedWaypointIndex).toBe(0);
  });

  it('keeps waypoint selection on a nearby item after removal', () => {
    const editor = makeEditor();
    editor.selectedWaypointIndex = 1;

    editor.removeWaypoint(1);

    expect(editor.editingWaypoints).toEqual(['A', 'C']);
    expect(editor.selectedWaypointIndex).toBe(1);
  });
});

describe('RouteEditor waypoint focus UI', () => {
  it('fires waypoint focus from rendered rows without marking data dirty', () => {
    document.body.innerHTML = '<div id="waypoints"></div>';
    const editor = Object.create(RouteEditor.prototype);
    editor.waypointListEl = document.getElementById('waypoints');
    editor.editingWaypoints = ['A'];
    editor.poiNames = ['A'];
    editor.selectedWaypointIndex = -1;
    editor.onWaypointFocus = vi.fn();
    editor.setDirty = vi.fn();

    editor.renderWaypointList();
    const row = editor.waypointListEl.querySelector('.wp-item');

    row.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    row.dispatchEvent(new Event('focus'));

    expect(editor.onWaypointFocus).toHaveBeenCalledWith('A');
    expect(editor.selectedWaypointIndex).toBe(0);
    expect(row.classList.contains('selected')).toBe(true);
    expect(editor.setDirty).not.toHaveBeenCalled();
  });

  it('does not fire waypoint focus from row hover', () => {
    document.body.innerHTML = '<div id="waypoints"></div>';
    const editor = Object.create(RouteEditor.prototype);
    editor.waypointListEl = document.getElementById('waypoints');
    editor.editingWaypoints = ['A'];
    editor.poiNames = ['A'];
    editor.selectedWaypointIndex = -1;
    editor.onWaypointFocus = vi.fn();

    editor.renderWaypointList();
    const row = editor.waypointListEl.querySelector('.wp-item');

    row.dispatchEvent(new MouseEvent('mouseenter', { bubbles: true }));

    expect(editor.onWaypointFocus).not.toHaveBeenCalled();
  });

  it('fires landmark focus from rendered rows without marking data dirty', () => {
    document.body.innerHTML = '<div id="landmarks"></div>';
    const editor = Object.create(RouteEditor.prototype);
    editor.landmarkListEl = document.getElementById('landmarks');
    editor.editingLandmarks = ['L1'];
    editor.landmarkNames = ['L1'];
    editor.onLandmarkFocus = vi.fn();
    editor.setDirty = vi.fn();

    editor.renderLandmarkList();
    const row = editor.landmarkListEl.querySelector('.wp-item');

    row.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    row.dispatchEvent(new Event('focus'));

    expect(editor.onLandmarkFocus).toHaveBeenCalledWith('L1');
    expect(editor.setDirty).not.toHaveBeenCalled();
  });

  it('does not fire landmark focus from row hover', () => {
    document.body.innerHTML = '<div id="landmarks"></div>';
    const editor = Object.create(RouteEditor.prototype);
    editor.landmarkListEl = document.getElementById('landmarks');
    editor.editingLandmarks = ['L1'];
    editor.landmarkNames = ['L1'];
    editor.onLandmarkFocus = vi.fn();

    editor.renderLandmarkList();
    const row = editor.landmarkListEl.querySelector('.wp-item');

    row.dispatchEvent(new MouseEvent('mouseenter', { bubbles: true }));

    expect(editor.onLandmarkFocus).not.toHaveBeenCalled();
  });

  it('moves a dragged waypoint to the drop target index', () => {
    const editor = makeEditor();
    editor.dragWaypointIndex = 0;

    editor._onWaypointDrop({ preventDefault: vi.fn() }, 2);

    expect(editor.editingWaypoints).toEqual(['B', 'C', 'A']);
    expect(editor.dragWaypointIndex).toBe(-1);
  });

  it('syncs waypoint and landmark selects from a POI candidate name', () => {
    const editor = Object.create(RouteEditor.prototype);
    editor.poiNames = ['A'];
    editor.landmarkNames = ['L1'];
    editor.waypointSelect = { value: '' };
    editor.landmarkSelect = { value: '' };
    editor.onWaypointFocus = vi.fn();
    editor.onLandmarkFocus = vi.fn();

    expect(editor.selectPoiCandidate('A')).toBe('waypoint');
    expect(editor.waypointSelect.value).toBe('A');
    expect(editor.landmarkSelect.value).toBe('');
    expect(editor.onWaypointFocus).toHaveBeenCalledWith('A');

    expect(editor.selectPoiCandidate('L1')).toBe('landmark');
    expect(editor.waypointSelect.value).toBe('');
    expect(editor.landmarkSelect.value).toBe('L1');
    expect(editor.onLandmarkFocus).toHaveBeenCalledWith('L1');

    expect(editor.selectPoiCandidate('missing')).toBe('');
  });

  it('keeps map insertion disabled by default for each edit session', () => {
    const editor = Object.create(RouteEditor.prototype);
    editor.formEl = document.createElement('div');
    editor.mapInsertMode = true;
    editor.mapInsertToggle = { checked: true };

    editor.showForm();

    expect(editor.isMapInsertModeEnabled()).toBe(false);
    expect(editor.mapInsertToggle.checked).toBe(false);
  });
});
