import type { PlayerObject } from '@/api/queries/players';

// Names for the object and group ids that audit rows carry. The tree is what the person is
// allowed to see, so a place outside it (the source of a move, or one since deleted) has no
// name here: it is shown as "another place" rather than guessed or looked up around the rights.

export interface PlaceIndex {
  object: Map<number, string>;
  group: Map<number, { name: string; objectId: number; objectName: string }>;
}

export function placeIndex(objects: PlayerObject[] | undefined): PlaceIndex {
  const idx: PlaceIndex = { object: new Map(), group: new Map() };
  for (const o of objects ?? []) {
    idx.object.set(o.id, o.name);
    for (const g of o.groups) idx.group.set(g.id, { name: g.name, objectId: o.id, objectName: o.name });
  }
  return idx;
}

// "Object / Group", "Object", or `unknown` when neither id is known to this person.
export function placeText(idx: PlaceIndex, objectId: number | undefined, groupId: number | undefined, unknown: string): string {
  if (groupId != null) {
    const g = idx.group.get(groupId);
    if (g) return `${g.objectName} / ${g.name}`;
  }
  if (objectId != null) {
    const o = idx.object.get(objectId);
    if (o) return o;
  }
  return groupId == null && objectId == null ? '' : unknown;
}
