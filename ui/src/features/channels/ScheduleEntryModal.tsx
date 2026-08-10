import React from 'react';
import { useTranslation } from 'react-i18next';
import { X, FileText, Trash2, Plus } from 'lucide-react';
import type { ScheduleEntry, ScheduleTransition, Recurrence } from '@/api/types';
import { FilePickerModal } from '@/components/FilePickerModal';
import { useEscClose } from '@/hooks/useEscClose';

const MEDIA_EXTS = [
  '.png', '.jpg', '.jpeg', '.webp', '.bmp',
  '.mp4', '.mov', '.mkv', '.avi', '.ts', '.m2ts', '.webm',
];

type RecurrenceKind = Recurrence['kind'];

interface Props {
  initial?: ScheduleEntry;
  existingIds: string[];
  onSubmit: (entry: ScheduleEntry) => void;
  onCancel: () => void;
  submitting?: boolean;
}

const TRANSITION_TYPES: ScheduleTransition['type'][] = [
  'crossfade', 'wipe_left', 'wipe_right', 'wipe_up', 'wipe_down',
  'push_left', 'push_right', 'push_up', 'push_down',
  'dissolve', 'fade_black', 'hardcut',
];
const TRANSITION_MODES: ScheduleTransition['mode'][] = ['hard_cut', 'freeze_fade', 'live_mix'];
const EASINGS: ScheduleTransition['easing'][] = ['linear', 'ease_in', 'ease_out', 'ease_in_out'];

const DOW = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

function defaultEntry(): ScheduleEntry {
  return {
    id: '',
    playlist: [],
    transition: { type: 'crossfade', mode: 'hard_cut', duration: 1, easing: 'linear' },
    recurrence: { kind: 'daily', start_time: '08:00', end_time: '20:00' },
    priority: 100,
    loop_mode: 'loop',
    hard_switch: false,
  };
}

// datetime-local <input> gives YYYY-MM-DDTHH:MM (no seconds, no TZ).
// Backend wants ISO8601 UTC ending in :SSZ. Strip Z+seconds for display, add for submit.
function isoToLocal(iso: string): string {
  if (!iso) return '';
  return iso.replace('Z', '').slice(0, 16);
}
function localToIso(local: string): string {
  if (!local) return '';
  return local.length === 16 ? `${local}:00Z` : local;
}

export function ScheduleEntryModal({ initial, existingIds, onSubmit, onCancel, submitting }: Props) {
  const { t } = useTranslation();
  const [entry, setEntry] = React.useState<ScheduleEntry>(initial ?? defaultEntry());
  const [newPath, setNewPath] = React.useState('');
  const [pickFile, setPickFile] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);
  const isEdit = !!initial;

  const setField = <K extends keyof ScheduleEntry>(k: K, v: ScheduleEntry[K]) =>
    setEntry(prev => ({ ...prev, [k]: v }));

  const setTransition = <K extends keyof ScheduleTransition>(k: K, v: ScheduleTransition[K]) =>
    setEntry(prev => ({ ...prev, transition: { ...prev.transition, [k]: v } }));

  // Switch recurrence kind, resetting to sane defaults for the new shape.
  const setRecurrenceKind = (kind: RecurrenceKind) => {
    let r: Recurrence;
    switch (kind) {
      case 'once':
        r = { kind: 'once', start_at: '', end_at: '' };
        break;
      case 'daily':
        r = { kind: 'daily', start_time: '08:00', end_time: '20:00' };
        break;
      case 'weekly':
        r = { kind: 'weekly', start_time: '08:00', end_time: '20:00', days_of_week: [] };
        break;
      case 'monthly':
        r = { kind: 'monthly', start_time: '08:00', end_time: '20:00', days_of_month: [] };
        break;
    }
    setEntry(prev => ({ ...prev, recurrence: r }));
  };

  const addPath = (p: string) => {
    const path = p.trim();
    if (!path) return;
    setEntry(prev => ({ ...prev, playlist: [...prev.playlist, path] }));
    setNewPath('');
  };
  const removePath = (idx: number) =>
    setEntry(prev => ({ ...prev, playlist: prev.playlist.filter((_, i) => i !== idx) }));

  const validate = (): string | null => {
    if (!entry.id.trim()) return 'ID is required';
    if (!/^[a-zA-Z0-9_-]+$/.test(entry.id)) return 'ID: letters, digits, _ and - only';
    if (!isEdit && existingIds.includes(entry.id)) return `Entry "${entry.id}" already exists`;
    if (entry.playlist.length === 0) return 'At least one clip path required';
    if (entry.priority < 0 || entry.priority > 1000) return 'Priority must be 0-1000';
    if (entry.transition.duration < 0) return 'Transition duration must be >= 0';

    const r = entry.recurrence;
    if (r.kind === 'once') {
      if (!r.start_at || !r.end_at) return 'Start and end datetime required for once';
    } else if (r.kind === 'weekly') {
      if (r.days_of_week.length === 0) return 'Pick at least one day of week';
    } else if (r.kind === 'monthly') {
      if (r.days_of_month.length === 0) return 'Pick at least one day of month';
    }
    return null;
  };

  const handleSave = () => {
    const err = validate();
    if (err) { setError(err); return; }
    setError(null);
    onSubmit(entry);
  };

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors';
  const labelCls = 'text-xs font-medium text-[var(--text-muted)] uppercase tracking-wider';

  useEscClose(onCancel, !submitting);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-2xl shadow-2xl flex flex-col"
           style={{ maxHeight: '90vh' }}>
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">
            {isEdit ? t('schedule.editTitle') : t('schedule.addTitle')}
          </h2>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>

        <div className="flex-1 overflow-y-auto p-5 flex flex-col gap-5">
          {error && (
            <div className="p-3 bg-[var(--danger)]/10 border border-[var(--danger)]/30 rounded-md text-sm text-[var(--danger)]">
              {error}
            </div>
          )}

          {/* Basics */}
          <div className="grid grid-cols-2 gap-4">
            <div className="flex flex-col gap-1.5">
              <label className={labelCls}>{t('schedule.fieldId')}</label>
              <input value={entry.id} onChange={e => setField('id', e.target.value)} disabled={isEdit}
                placeholder="morning-block" className={`${inputCls} disabled:opacity-50`} />
            </div>
            <div className="flex flex-col gap-1.5">
              <label className={labelCls}>{t('schedule.fieldPriority')}</label>
              <input type="number" min={0} max={1000} value={entry.priority}
                onChange={e => setField('priority', parseInt(e.target.value || '0'))}
                className={inputCls} />
            </div>
            <div className="flex flex-col gap-1.5">
              <label className={labelCls}>{t('schedule.fieldLoopMode')}</label>
              <select value={entry.loop_mode} onChange={e => setField('loop_mode', e.target.value as ScheduleEntry['loop_mode'])}
                className={inputCls}>
                <option value="loop">loop</option>
                <option value="play_once_then_idle">play_once_then_idle</option>
              </select>
            </div>
            <div className="flex flex-col gap-1.5 justify-end pb-2">
              <label className="flex items-center gap-2 text-sm text-[var(--text-primary)] cursor-pointer">
                <input type="checkbox" checked={entry.hard_switch}
                  onChange={e => setField('hard_switch', e.target.checked)} />
                {t('schedule.fieldHardSwitch')}
              </label>
            </div>
            <div className="flex flex-col gap-1.5">
              <label className={labelCls}>{t('schedule.fieldEffectiveFrom')}</label>
              <input type="date" value={entry.effective_from ?? ''}
                onChange={e => setField('effective_from', e.target.value || undefined)}
                className={inputCls} />
            </div>
            <div className="flex flex-col gap-1.5">
              <label className={labelCls}>{t('schedule.fieldEffectiveTo')}</label>
              <input type="date" value={entry.effective_to ?? ''}
                onChange={e => setField('effective_to', e.target.value || undefined)}
                className={inputCls} />
            </div>
          </div>

          {/* Playlist */}
          <div className="flex flex-col gap-2">
            <label className={labelCls}>{t('schedule.fieldPlaylist')}</label>
            {entry.playlist.length > 0 && (
              <div className="flex flex-col gap-1 border border-[var(--border-subtle)] rounded-md p-2 bg-canvas">
                {entry.playlist.map((p, i) => (
                  <div key={i} className="flex items-center gap-2 text-sm">
                    <span className="text-xs text-[var(--text-muted)] w-5 text-right tabular-nums">{i + 1}</span>
                    <span className="flex-1 font-mono text-[var(--text-primary)] truncate">{p}</span>
                    <button onClick={() => removePath(i)}
                      className="p-1 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                      <Trash2 size={13} />
                    </button>
                  </div>
                ))}
              </div>
            )}
            <div className="flex gap-2">
              <input value={newPath} onChange={e => setNewPath(e.target.value)}
                placeholder="promo/clip.mp4"
                onKeyDown={e => { if (e.key === 'Enter') { e.preventDefault(); addPath(newPath); } }}
                className={`${inputCls} flex-1`} />
              <button type="button" onClick={() => setPickFile(true)}
                className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                <FileText size={14} /> {t('common.browse')}
              </button>
              <button type="button" onClick={() => addPath(newPath)} disabled={!newPath.trim()}
                className="flex items-center gap-1 px-3 py-2 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md disabled:opacity-40 transition-colors">
                <Plus size={14} /> {t('common.add')}
              </button>
            </div>
          </div>

          {/* Transition */}
          <div className="flex flex-col gap-2">
            <label className={labelCls}>{t('schedule.fieldTransition')}</label>
            <div className="grid grid-cols-2 gap-3">
              <select value={entry.transition.type} onChange={e => setTransition('type', e.target.value as ScheduleTransition['type'])}
                className={inputCls}>
                {TRANSITION_TYPES.map(v => <option key={v} value={v}>{v}</option>)}
              </select>
              <select value={entry.transition.mode} onChange={e => setTransition('mode', e.target.value as ScheduleTransition['mode'])}
                className={inputCls}>
                {TRANSITION_MODES.map(v => <option key={v} value={v}>{v}</option>)}
              </select>
              <input type="number" step="0.1" min={0} value={entry.transition.duration}
                onChange={e => setTransition('duration', parseFloat(e.target.value || '0'))}
                placeholder={t('schedule.transitionDuration')}
                className={inputCls} />
              <select value={entry.transition.easing} onChange={e => setTransition('easing', e.target.value as ScheduleTransition['easing'])}
                className={inputCls}>
                {EASINGS.map(v => <option key={v} value={v}>{v}</option>)}
              </select>
            </div>
          </div>

          {/* Recurrence */}
          <div className="flex flex-col gap-2">
            <label className={labelCls}>{t('schedule.fieldRecurrence')}</label>
            <div className="flex gap-2">
              {(['once', 'daily', 'weekly', 'monthly'] as RecurrenceKind[]).map(k => (
                <button key={k} type="button" onClick={() => setRecurrenceKind(k)}
                  className={`px-3 py-1.5 text-sm rounded-md transition-colors ${
                    entry.recurrence.kind === k
                      ? 'bg-[var(--accent)] text-white'
                      : 'border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
                  }`}>
                  {k}
                </button>
              ))}
            </div>

            {entry.recurrence.kind === 'once' && (
              <div className="grid grid-cols-2 gap-3 mt-2">
                <div className="flex flex-col gap-1.5">
                  <label className="text-xs text-[var(--text-muted)]">{t('schedule.startAt')} (UTC)</label>
                  <input type="datetime-local" value={isoToLocal(entry.recurrence.start_at)}
                    onChange={e => setEntry(prev => ({ ...prev, recurrence: { ...prev.recurrence, kind: 'once', start_at: localToIso(e.target.value), end_at: prev.recurrence.kind === 'once' ? prev.recurrence.end_at : '' } }))}
                    className={inputCls} />
                </div>
                <div className="flex flex-col gap-1.5">
                  <label className="text-xs text-[var(--text-muted)]">{t('schedule.endAt')} (UTC)</label>
                  <input type="datetime-local" value={isoToLocal(entry.recurrence.end_at)}
                    onChange={e => setEntry(prev => ({ ...prev, recurrence: { ...prev.recurrence, kind: 'once', start_at: prev.recurrence.kind === 'once' ? prev.recurrence.start_at : '', end_at: localToIso(e.target.value) } }))}
                    className={inputCls} />
                </div>
              </div>
            )}

            {(entry.recurrence.kind === 'daily' || entry.recurrence.kind === 'weekly' || entry.recurrence.kind === 'monthly') && (
              <div className="grid grid-cols-2 gap-3 mt-2">
                <div className="flex flex-col gap-1.5">
                  <label className="text-xs text-[var(--text-muted)]">{t('schedule.startTime')} (UTC)</label>
                  <input type="time" value={entry.recurrence.start_time}
                    onChange={e => setEntry(prev => ({ ...prev, recurrence: { ...prev.recurrence, start_time: e.target.value } as Recurrence }))}
                    className={inputCls} />
                </div>
                <div className="flex flex-col gap-1.5">
                  <label className="text-xs text-[var(--text-muted)]">{t('schedule.endTime')} (UTC)</label>
                  <input type="time" value={entry.recurrence.end_time}
                    onChange={e => setEntry(prev => ({ ...prev, recurrence: { ...prev.recurrence, end_time: e.target.value } as Recurrence }))}
                    className={inputCls} />
                </div>
              </div>
            )}

            {entry.recurrence.kind === 'weekly' && (
              <div className="flex flex-col gap-1.5 mt-2">
                <label className="text-xs text-[var(--text-muted)]">{t('schedule.daysOfWeek')}</label>
                <div className="flex gap-1.5 flex-wrap">
                  {DOW.map((name, i) => {
                    const r = entry.recurrence;
                    if (r.kind !== 'weekly') return null;
                    const active = r.days_of_week.includes(i);
                    return (
                      <button key={i} type="button"
                        onClick={() => {
                          const next = active
                            ? r.days_of_week.filter(d => d !== i)
                            : [...r.days_of_week, i].sort();
                          setEntry(prev => ({ ...prev, recurrence: { ...r, days_of_week: next } }));
                        }}
                        className={`px-2.5 py-1 text-xs rounded-md transition-colors ${
                          active
                            ? 'bg-[var(--accent)] text-white'
                            : 'border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
                        }`}>
                        {name}
                      </button>
                    );
                  })}
                </div>
              </div>
            )}

            {entry.recurrence.kind === 'monthly' && (
              <div className="flex flex-col gap-1.5 mt-2">
                <label className="text-xs text-[var(--text-muted)]">{t('schedule.daysOfMonth')}</label>
                <div className="grid grid-cols-10 gap-1">
                  {Array.from({ length: 31 }, (_, i) => i + 1).map(day => {
                    const r = entry.recurrence;
                    if (r.kind !== 'monthly') return null;
                    const active = r.days_of_month.includes(day);
                    return (
                      <button key={day} type="button"
                        onClick={() => {
                          const next = active
                            ? r.days_of_month.filter(d => d !== day)
                            : [...r.days_of_month, day].sort((a, b) => a - b);
                          setEntry(prev => ({ ...prev, recurrence: { ...r, days_of_month: next } }));
                        }}
                        className={`px-1 py-1 text-xs rounded transition-colors ${
                          active
                            ? 'bg-[var(--accent)] text-white'
                            : 'border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
                        }`}>
                        {day}
                      </button>
                    );
                  })}
                </div>
              </div>
            )}
          </div>
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
            className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={handleSave} disabled={submitting}
            className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
            {submitting ? t('common.loading') : t('common.save')}
          </button>
        </div>
      </div>

      {pickFile && (
        <FilePickerModal
          initialPath={newPath?.replace(/\/[^/]*$/, '') || '/'}
          acceptExtensions={MEDIA_EXTS}
          onSelect={(p) => { addPath(p); setPickFile(false); }}
          onCancel={() => setPickFile(false)}
        />
      )}
    </div>
  );
}
