import { useTranslation } from 'react-i18next';
import type { PlayerObject } from '@/api/queries/players';
import { inputCls } from './fields';

// Pick an object, and then one of its groups, to narrow a list to a place. With `onlyAudit` only
// the places whose journal the person may read are offered (the server refuses the rest anyway).

export interface PlaceValue { objectId?: number; groupId?: number }

export function PlaceFilter({ objects, value, onChange, onlyAudit }: {
  objects: PlayerObject[]; value: PlaceValue; onChange: (v: PlaceValue) => void; onlyAudit?: boolean;
}) {
  const { t } = useTranslation();
  // A group right opens that group only; an object right opens every group of the object.
  const objectOpen = (o: PlayerObject) => !onlyAudit || o.can?.view_audit === true;
  const groupsOf = (o: PlayerObject) => o.groups.filter(g => !onlyAudit || objectOpen(o) || g.can?.view_audit === true);
  const shown = objects.filter(o => objectOpen(o) || groupsOf(o).length > 0);
  const current = shown.find(o => o.id === value.objectId);

  return (
    <div className="flex flex-wrap gap-2">
      <select aria-label={t('players.place.object')} value={value.objectId ?? ''} className={inputCls.replace('w-full', 'w-48')}
              onChange={e => onChange({ objectId: e.target.value ? Number(e.target.value) : undefined })}>
        <option value="">{t('players.place.allObjects')}</option>
        {shown.map(o => <option key={o.id} value={o.id}>{o.name}</option>)}
      </select>
      <select aria-label={t('players.place.group')} value={value.groupId ?? ''} className={inputCls.replace('w-full', 'w-48')}
              disabled={!current}
              onChange={e => onChange({ objectId: value.objectId, groupId: e.target.value ? Number(e.target.value) : undefined })}>
        <option value="">{t('players.place.allGroups')}</option>
        {current && groupsOf(current).map(g => <option key={g.id} value={g.id}>{g.name}</option>)}
      </select>
    </div>
  );
}
