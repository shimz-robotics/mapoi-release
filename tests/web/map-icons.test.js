// Unit tests for the Leaflet-independent pure helpers extracted from map-viewer.js
// into map-icons.js (#346): tag/route color resolution and route direction math.
// createArrowIcon / createRobotIcon / createRouteLandmarkIcon call `L.divIcon` and are
// exercised only through Playwright e2e (Leaflet is loaded from a CDN `<script>`, not an
// npm dependency here), consistent with how map-viewer.js itself has never been unit
// tested for its Leaflet-touching methods.
import { describe, it, expect } from 'vitest';
import * as icons from '../../web/js/map-icons.js';

describe('getPoiColor', () => {
  it('returns the color of the first matching tag', () => {
    const tagColors = { waypoint: '#111111', landmark: '#222222' };
    expect(icons.getPoiColor(tagColors, { tags: ['landmark', 'waypoint'] })).toBe('#222222');
  });

  it('falls back to the default blue when no tag matches', () => {
    expect(icons.getPoiColor({}, { tags: ['unknown'] })).toBe('#3498db');
    expect(icons.getPoiColor({}, {})).toBe('#3498db');
  });
});

describe('getRouteColor', () => {
  it('uses the fixed palette for indices within range', () => {
    expect(icons.getRouteColor(0)).toBe('#2980b9');
    expect(icons.getRouteColor(5)).toBe('#7f8c8d');
  });

  it('falls back to an HSL color once the palette is exhausted', () => {
    expect(icons.getRouteColor(6)).toBe('hsl(30, 55%, 45%)');
  });
});

describe('routeDirectionDeg', () => {
  it('computes 0 degrees for a segment pointing straight up (increasing lat, same lng)', () => {
    expect(icons.routeDirectionDeg({ lat: 0, lng: 0 }, { lat: 1, lng: 0 })).toBeCloseTo(0);
  });

  it('computes 90 degrees for a segment pointing right (same lat, increasing lng)', () => {
    expect(icons.routeDirectionDeg({ lat: 0, lng: 0 }, { lat: 0, lng: 1 })).toBeCloseTo(90);
  });
});
