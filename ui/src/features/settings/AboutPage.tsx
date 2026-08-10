import { useTranslation } from 'react-i18next';
import { useVersion } from '@/api/queries/system';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { Check, X } from 'lucide-react';

export default function AboutPage() {
  const { t } = useTranslation();
  const { data: v, isLoading } = useVersion();
  if (isLoading) return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  if (!v) return null;
  const attributions = v.attributions ?? [];
  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('about.build')}</h2>
        <dl className="grid grid-cols-3 gap-x-6 gap-y-5">
          {[
            [t('about.version'),   v.version],
            [t('about.buildTime'), v.build_time],
            [t('about.buildType'), v.build_type],
          ].map(([k, val]) => (
            <div key={k}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
              <dd className="text-sm font-mono text-[var(--text-primary)]">{val ?? '—'}</dd>
            </div>
          ))}
        </dl>
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('about.features')}</h2>
        <div className="grid grid-cols-4 gap-x-6 gap-y-3">
          {Object.entries(v.features ?? {}).map(([k, enabled]) => (
            <div key={k} className="flex items-center gap-2 text-sm">
              {enabled ? <Check size={14} className="text-[var(--success)] shrink-0" />
                       : <X     size={14} className="text-[var(--text-muted)] shrink-0" />}
              <span className={`font-mono ${enabled ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>{k}</span>
            </div>
          ))}
        </div>
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('about.thirdPartyNotices')}</h2>
        {attributions.length === 0 ? (
          <p className="text-sm text-[var(--text-muted)]">{t('about.noAttributions')}</p>
        ) : (
          <div className="flex flex-col">
            {attributions.map((a, i) => (
              <div key={i}
                   className="text-sm text-[var(--text-muted)] py-2 border-b last:border-0 border-[var(--border-subtle)]">
                {a}
              </div>
            ))}
          </div>
        )}
      </Block>
    </div>
  );
}
