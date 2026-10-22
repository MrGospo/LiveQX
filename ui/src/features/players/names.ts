// The rule the server applies to every name in the screens module (an object, a
// group, a screen): spaces at the ends are cut, the rest is not empty, has no
// control characters and fits in 120 bytes (not letters: a Russian letter is two).

export const MAX_NAME_BYTES = 120;

export type NameProblem = 'empty' | 'tooLong' | 'badChars';

export const trimName = (raw: string) => raw.replace(/^ +| +$/g, '');

export function nameProblem(raw: string): NameProblem | null {
  const s = trimName(raw);
  if (!s) return 'empty';
  if (new TextEncoder().encode(s).length > MAX_NAME_BYTES) return 'tooLong';
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f\u007f]/.test(s)) return 'badChars';
  return null;
}
