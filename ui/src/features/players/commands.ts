import { COMMAND_TYPES, type BulkTarget, type CommandType } from '@/api/queries/players';

// Which commands a person may aim at a place, by the same rules the server applies (the
// server decides every send; this only keeps the interface from offering what it will refuse):
//   * a group: with the "command a group" mark any command; without it, an operator with access
//     may still reload the playlist;
//   * an object: only with the "command an object" mark (the group mark does not replace it);
//   * everyone: administrators only;
//   * a viewer never commands.

export type Role = 'viewer' | 'operator' | 'admin' | null;

export function typesForTarget(
  target: BulkTarget,
  role: Role,
  can: { commandGroup?: boolean; commandObject?: boolean },
): CommandType[] {
  if (role !== 'operator' && role !== 'admin') return [];
  if (target.kind === 'all') return role === 'admin' ? COMMAND_TYPES : [];
  if (target.kind === 'object') return can.commandObject ? COMMAND_TYPES : [];
  return can.commandGroup ? COMMAND_TYPES : ['reload_playlist'];
}

// The server compares the typed phrase without regard to case and spaces; the button
// follows the same rule so it does not wait for something the server would accept.
const squash = (s: string) => s.toLowerCase().replace(/\s+/g, '');
export const phraseMatches = (typed: string, required: string) => squash(typed) === squash(required);
