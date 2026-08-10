import { useTranslation } from 'react-i18next';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { Check } from 'lucide-react';
import { useMasterKeyInfo } from '@/api/queries/auth';

function fmtUnix(unixSec: number | null | undefined): string {
  if (unixSec == null || unixSec === 0) return '—';
  return new Date(unixSec * 1000).toLocaleString();
}

// Backend в Info.source выдаёт "file" | "env" | "env_file" — карта на
// человекочитаемые i18n-метки. Неизвестный источник не маппим, чтобы
// surface'нуть рассинхрон между UI и backend (вместо тихой подмены).
function sourceI18nKey(source: string): string {
  switch (source) {
    case 'file':     return 'masterKey.sourceFile';
    case 'env':      return 'masterKey.sourceEnv';
    case 'env_file': return 'masterKey.sourceEnvFile';
    default:         return 'masterKey.sourceUnknown';
  }
}

export default function MasterKeyPage() {
  const { t } = useTranslation();
  const { data, isLoading, error } = useMasterKeyInfo();

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{t('masterKey.keyInfo')}</h2>
        {isLoading ? (
          <div className="h-32 bg-surface2 rounded-md animate-pulse" />
        ) : error ? (
          <div className="text-sm text-[var(--danger)]">{(error as Error).message}</div>
        ) : data ? (
          <dl className="grid grid-cols-2 gap-4 text-sm">
            <div>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('masterKey.fingerprint')}</dt>
              <dd className="text-[var(--text-primary)] font-mono break-all">{data.fingerprint}</dd>
            </div>
            <div>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('masterKey.algorithm')}</dt>
              <dd className="text-[var(--text-primary)] font-mono">{data.algorithm}</dd>
            </div>
            <div>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('masterKey.source')}</dt>
              <dd className="text-[var(--text-primary)]">
                {t(sourceI18nKey(data.source))}{' '}
                <span className="text-[var(--text-muted)] font-mono text-xs">({data.source})</span>
              </dd>
            </div>
            <div>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('masterKey.created')}</dt>
              <dd className="text-[var(--text-primary)] font-mono">{fmtUnix(data.created_at)}</dd>
            </div>
            <div>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('masterKey.lastRotated')}</dt>
              <dd className="text-[var(--text-primary)] font-mono">{fmtUnix(data.last_rotated_at)}</dd>
            </div>
          </dl>
        ) : null}
      </Block>
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('masterKey.encryptedSecrets')}</h2>
        <div className="flex flex-col gap-2">
          {['jwt_secret', 'ldap.bind_password', 'smtp.password'].map(s => (
            <div key={s} className="flex items-center gap-2 text-sm">
              <Check size={14} className="text-[var(--success)]" />
              <span className="font-mono text-[var(--text-primary)]">{s}</span>
              <span className="text-[var(--text-muted)]">(auth.db)</span>
            </div>
          ))}
        </div>
      </Block>
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{t('masterKey.rotation')}</h2>
        {data?.source === 'file' ? (
          <>
            <p className="text-sm text-[var(--text-muted)] mb-4">{t('masterKey.rotationHintFile')}</p>
            <div className="bg-canvas rounded-lg px-4 py-3 font-mono text-sm text-[var(--accent)] leading-relaxed">
              liveqx --rotate-master-key \<br />
              &nbsp;&nbsp;&nbsp;&nbsp;--auth-db state/auth.db \<br />
              &nbsp;&nbsp;&nbsp;&nbsp;--master-key-file state/master.key
            </div>
          </>
        ) : data?.source === 'env' ? (
          <p className="text-sm text-[var(--text-muted)]">{t('masterKey.rotationHintEnv')}</p>
        ) : data?.source === 'env_file' ? (
          <p className="text-sm text-[var(--text-muted)]">{t('masterKey.rotationHintEnvFile')}</p>
        ) : (
          <p className="text-sm text-[var(--text-muted)]">{t('masterKey.rotationHint')}</p>
        )}
      </Block>
    </div>
  );
}
