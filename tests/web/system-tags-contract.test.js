// Contract test for system tag names shared by mapoi_server and WebUI (#193).
// Runtime WebUI fetches tag definitions from mapoi_server, but these pure filter
// helpers still encode behavior for specific system tags. Keep that behavior
// tied to mapoi_server/include/mapoi_server/system_tags.hpp.

import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import * as filter from '../../web/js/poi-filter.js';

const { SYSTEM_TAGS, SYSTEM_TAG_NAMES } = filter;

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '../../..');
const systemTagsHeaderPath = path.join(
  repoRoot,
  'mapoi_server',
  'include',
  'mapoi_server',
  'system_tags.hpp',
);

function loadServerSystemTagNames() {
  const header = readFileSync(systemTagsHeaderPath, 'utf8');
  // Assumption: kSystemTags is the only header initializer that uses
  // {"name", "description"} entries. If this test fails after adding another
  // similar array, narrow this parser to the kSystemTags block.
  return [...header.matchAll(/\{\s*"([^"]+)"\s*,\s*"[^"]*"\s*\}/g)].map((match) => match[1]);
}

describe('system tag contract', () => {
  it('keeps WebUI pure helper constants aligned with mapoi_server system_tags.hpp', () => {
    expect(SYSTEM_TAG_NAMES).toEqual(loadServerSystemTagNames());
  });

  it('keeps behavior-bearing system tags explicit', () => {
    expect(SYSTEM_TAGS).toEqual({
      WAYPOINT: 'waypoint',
      LANDMARK: 'landmark',
      PAUSE: 'pause',
    });
  });
});
