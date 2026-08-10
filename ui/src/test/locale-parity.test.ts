/**
 * Locale parity — fails if en.json and ru.json have different key sets.
 *
 * Round 3 acceptance gate: every translated key must exist in both locales,
 * so a missing translation can never silently fall back to the key string.
 */
import { describe, it, expect } from 'vitest';
import en from '@/locales/en.json';
import ru from '@/locales/ru.json';

type AnyObject = Record<string, unknown>;

function flatten(obj: AnyObject, prefix = ''): string[] {
  const keys: string[] = [];
  for (const [k, v] of Object.entries(obj)) {
    const full = prefix ? `${prefix}.${k}` : k;
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      keys.push(...flatten(v as AnyObject, full));
    } else {
      keys.push(full);
    }
  }
  return keys.sort();
}

describe('locale parity', () => {
  const enKeys = flatten(en as AnyObject);
  const ruKeys = flatten(ru as AnyObject);

  it('en.json and ru.json have identical key sets', () => {
    const enSet = new Set(enKeys);
    const ruSet = new Set(ruKeys);
    const onlyEn = enKeys.filter((k) => !ruSet.has(k));
    const onlyRu = ruKeys.filter((k) => !enSet.has(k));
    expect({ onlyEn, onlyRu }).toEqual({ onlyEn: [], onlyRu: [] });
  });

  it('no empty translations', () => {
    function check(obj: AnyObject, prefix = ''): string[] {
      const empty: string[] = [];
      for (const [k, v] of Object.entries(obj)) {
        const full = prefix ? `${prefix}.${k}` : k;
        if (v && typeof v === 'object' && !Array.isArray(v)) {
          empty.push(...check(v as AnyObject, full));
        } else if (typeof v === 'string' && v.trim().length === 0) {
          empty.push(full);
        }
      }
      return empty;
    }
    expect(check(en as AnyObject)).toEqual([]);
    expect(check(ru as AnyObject)).toEqual([]);
  });
});
