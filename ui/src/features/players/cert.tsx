import { useTranslation } from 'react-i18next';
import { clsx } from 'clsx';
import type { CertStageName, PlayerCert } from '@/api/queries/players';
import { absoluteTime } from './format';

// How a screen's certificate is shown: in the list (one short phrase), on the card (a callout that says
// what to do), and for the server's own certificates (a step with a colour). The steps come from the
// server; here they only get words and a tone.

export const CERT_TONE: Record<CertStageName, string> = {
  ok:      'text-[var(--text-muted)]',
  soon30:  'text-[var(--warning)]',
  soon14:  'text-[var(--warning)]',
  soon3:   'text-[var(--danger)]',
  expired: 'text-[var(--danger)]',
};

export const CERT_STAGES: CertStageName[] = ['ok', 'soon30', 'soon14', 'soon3', 'expired'];

export const isCertStage = (v: unknown): v is CertStageName =>
  typeof v === 'string' && (CERT_STAGES as string[]).includes(v);

// "300 days left", "less than a day left", "ended".
export function useDaysLeftText() {
  const { t } = useTranslation();
  return (stage: CertStageName, daysLeft: number | undefined): string => {
    if (stage === 'expired') return t('players.cert.expired');
    if (daysLeft == null) return t(`players.cert.stage.${stage}`);
    return daysLeft <= 0 ? t('players.cert.lessThanDay') : t('players.cert.daysLeft', { count: daysLeft });
  };
}

export function CertCell({ cert }: { cert?: PlayerCert }) {
  const text = useDaysLeftText();
  if (!cert) return <span className="text-[var(--text-muted)]">—</span>;
  return (
    <span className={clsx('whitespace-nowrap', CERT_TONE[cert.stage])}
          title={cert.not_after ? absoluteTime(cert.not_after) : undefined}>
      {text(cert.stage, cert.days_left)}
    </span>
  );
}
