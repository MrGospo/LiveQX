import React from 'react';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { useTranslation } from 'react-i18next';
import { BookOpen, ChevronRight, FileText, Search, X } from 'lucide-react';

type CategoryKey = 'start' | 'channels' | 'outputs' | 'settings' | 'operations';

interface DocEntry {
  file: string;
  category: CategoryKey;
}

const DOCS: DocEntry[] = [
  // Начало работы / Getting started
  { file: 'QUICKSTART.md',          category: 'start' },
  { file: 'OVERVIEW.md',            category: 'start' },

  // Каналы / Channels
  { file: 'CREATE_CHANNEL.md',      category: 'channels' },
  { file: 'PLAYLIST_USER.md',       category: 'channels' },
  { file: 'CONTENT_SOURCE_USER.md', category: 'channels' },
  { file: 'TRANSITIONS_USER.md',    category: 'channels' },
  { file: 'SCHEDULE_USER.md',       category: 'channels' },
  { file: 'DASHBOARD_USER.md',      category: 'channels' },

  // Выходы / Outputs
  { file: 'OUTPUTS_OVERVIEW.md',    category: 'outputs' },
  { file: 'ADD_OUTPUT.md',          category: 'outputs' },
  { file: 'LIVE_INPUT_USER.md',     category: 'outputs' },

  // Настройки / Settings
  { file: 'PROFILE_USER.md',        category: 'settings' },
  { file: 'USERS_USER.md',          category: 'settings' },
  { file: 'TIME_USER.md',           category: 'settings' },
  { file: 'MOUNTS_USER.md',         category: 'settings' },

  // Операции / Operations
  { file: 'STRESS_USER.md',         category: 'operations' },
  { file: 'TROUBLESHOOTING.md',     category: 'operations' },
];

const CATEGORY_ICONS: Record<CategoryKey, string> = {
  start:      '🚀',
  channels:   '📺',
  outputs:    '📡',
  settings:   '⚙️',
  operations: '🛠️',
};

const CATEGORY_ORDER: CategoryKey[] = ['start', 'channels', 'outputs', 'settings', 'operations'];

function docKey(file: string): string {
  return file.replace(/\.md$/i, '');
}

function pickLang(lng: string): 'ru' | 'en' {
  return lng.toLowerCase().startsWith('ru') ? 'ru' : 'en';
}

export default function DocsPage() {
  const { t, i18n } = useTranslation();
  const lang = pickLang(i18n.language);

  const [selected, setSelected] = React.useState<DocEntry | null>(null);
  const [content, setContent] = React.useState<string>('');
  const [loading, setLoading] = React.useState(false);
  const [search, setSearch] = React.useState('');

  const fetchDoc = React.useCallback(async (doc: DocEntry, useLang: 'ru' | 'en') => {
    setLoading(true);
    setContent('');
    try {
      const res = await fetch(`/docs/${useLang}/${doc.file}`);
      if (res.ok) {
        setContent(await res.text());
      } else if (useLang === 'en') {
        const fb = await fetch(`/docs/ru/${doc.file}`);
        setContent(fb.ok ? await fb.text() : `${t('docs.loadError')} ${doc.file}`);
      } else {
        setContent(`${t('docs.loadError')} ${doc.file}`);
      }
    } catch {
      setContent(t('docs.fetchError'));
    } finally {
      setLoading(false);
    }
  }, [t]);

  const open = (doc: DocEntry) => {
    setSelected(doc);
    fetchDoc(doc, lang);
  };

  React.useEffect(() => {
    if (selected) fetchDoc(selected, lang);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [lang]);

  const docTitle = (d: DocEntry) => t(`docs.items.${docKey(d.file)}.title`);
  const docDesc  = (d: DocEntry) => t(`docs.items.${docKey(d.file)}.desc`);
  const catTitle = (c: CategoryKey) => t(`docs.category.${c}`);

  const q = search.trim().toLowerCase();
  const filtered = q
    ? DOCS.filter(d =>
        docTitle(d).toLowerCase().includes(q) ||
        docDesc(d).toLowerCase().includes(q) ||
        catTitle(d.category).toLowerCase().includes(q))
    : DOCS;

  if (selected) {
    return (
      <div className="flex flex-col h-full">
        {/* Header */}
        <div className="flex items-center gap-3 px-7 py-4 border-b border-[var(--border-subtle)] flex-shrink-0">
          <button onClick={() => setSelected(null)}
            className="flex items-center gap-1.5 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
            <BookOpen size={15} />
            <span>{t('docs.title')}</span>
          </button>
          <ChevronRight size={14} className="text-[var(--text-muted)]" />
          <span className="text-sm font-medium text-[var(--text-primary)]">{docTitle(selected)}</span>
          <div className="flex-1" />
          <button onClick={() => setSelected(null)}
            className="p-1.5 text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors rounded">
            <X size={16} />
          </button>
        </div>

        {/* Markdown content */}
        <div className="flex-1 overflow-y-auto">
          <article className="max-w-3xl w-full mx-auto px-8 py-8 text-[15px] leading-7 text-[var(--text-primary)]">
            {loading ? (
              <div className="flex flex-col gap-3 animate-pulse">
                {Array(8).fill(0).map((_, i) => (
                  <div key={i} className={`h-4 bg-surface2 rounded ${i % 3 === 0 ? 'w-2/5' : i % 2 === 0 ? 'w-4/5' : 'w-full'}`} />
                ))}
              </div>
            ) : (
              <ReactMarkdown
                remarkPlugins={[remarkGfm]}
                components={{
                  h1: ({ children }) => <h1 className="text-3xl font-bold text-[var(--text-primary)] mt-2 mb-6 pb-3 border-b border-[var(--border-subtle)] tracking-tight">{children}</h1>,
                  h2: ({ children }) => <h2 className="text-xl font-semibold text-[var(--text-primary)] mt-10 mb-4 pb-2 border-b border-[var(--border-subtle)]/60">{children}</h2>,
                  h3: ({ children }) => <h3 className="text-lg font-semibold text-[var(--text-primary)] mt-7 mb-3">{children}</h3>,
                  h4: ({ children }) => <h4 className="text-base font-semibold text-[var(--text-primary)] mt-5 mb-2">{children}</h4>,
                  p:  ({ children }) => <p className="my-4 leading-7">{children}</p>,
                  code: ({ className, children, ...props }) => {
                    const isBlock = className?.startsWith('language-');
                    return isBlock
                      ? <code className={`${className} block`} {...props}>{children}</code>
                      : <code className="bg-surface2 border border-[var(--border-subtle)] rounded px-1.5 py-0.5 text-[0.85em] font-mono text-[var(--accent)] whitespace-nowrap" {...props}>{children}</code>;
                  },
                  pre: ({ children }) => (
                    <pre className="my-5 bg-canvas border border-[var(--border-subtle)] rounded-lg px-4 py-3 text-[13px] leading-6 font-mono text-[var(--text-primary)] overflow-x-auto">
                      {children}
                    </pre>
                  ),
                  ul: ({ children }) => <ul className="my-4 pl-6 list-disc space-y-1.5 marker:text-[var(--text-muted)]">{children}</ul>,
                  ol: ({ children }) => <ol className="my-4 pl-6 list-decimal space-y-1.5 marker:text-[var(--text-muted)]">{children}</ol>,
                  li: ({ children }) => <li className="leading-7 pl-1">{children}</li>,
                  a:  ({ children, href }) => {
                    const linkClass = "text-[var(--accent)] underline decoration-[var(--accent)]/40 hover:decoration-[var(--accent)] underline-offset-2";
                    if (href) {
                      const normalized = href.replace(/^\.\//, '').split('#')[0];
                      const internal = DOCS.find(d => d.file === normalized);
                      if (internal) {
                        return (
                          <a
                            href={href}
                            className={linkClass}
                            onClick={e => { e.preventDefault(); open(internal); }}
                          >{children}</a>
                        );
                      }
                      if (href.startsWith('#')) {
                        return <a href={href} className={linkClass}>{children}</a>;
                      }
                    }
                    return <a href={href} className={linkClass} target="_blank" rel="noreferrer">{children}</a>;
                  },
                  blockquote: ({ children }) => (
                    <blockquote className="my-5 border-l-4 border-[var(--accent)]/50 bg-surface/40 rounded-r px-4 py-2 text-[var(--text-muted)]">
                      {children}
                    </blockquote>
                  ),
                  table: ({ children }) => (
                    <div className="my-5 overflow-x-auto border border-[var(--border-subtle)] rounded-lg">
                      <table className="w-full text-sm border-collapse">{children}</table>
                    </div>
                  ),
                  thead: ({ children }) => <thead className="bg-surface2">{children}</thead>,
                  tbody: ({ children }) => <tbody>{children}</tbody>,
                  tr: ({ children }) => <tr className="border-b border-[var(--border-subtle)]/60 last:border-0 even:bg-surface/30">{children}</tr>,
                  th: ({ children }) => <th className="px-3 py-2 text-left text-[11px] font-semibold uppercase tracking-wider text-[var(--text-muted)] border-r border-[var(--border-subtle)]/40 last:border-0 align-top">{children}</th>,
                  td: ({ children }) => <td className="px-3 py-2 text-sm align-top border-r border-[var(--border-subtle)]/30 last:border-0 break-words">{children}</td>,
                  hr: () => <hr className="border-[var(--border-subtle)] my-8" />,
                  strong: ({ children }) => <strong className="font-semibold text-[var(--text-primary)]">{children}</strong>,
                  em: ({ children }) => <em className="italic text-[var(--text-primary)]">{children}</em>,
                  img: ({ src, alt }) => <img src={src} alt={alt} className="my-5 rounded-lg border border-[var(--border-subtle)] max-w-full" />,
                }}
              >
                {content}
              </ReactMarkdown>
            )}
          </article>
        </div>
      </div>
    );
  }

  return (
    <div className="p-7 flex flex-col gap-6">
      {/* Header */}
      <div className="flex items-center gap-3">
        <BookOpen size={22} className="text-[var(--accent)]" />
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('docs.title')}</h1>
      </div>

      {/* Search */}
      <div className="flex items-center justify-end gap-4 flex-wrap">
        <div className="relative max-w-sm flex-1">
          <Search size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-[var(--text-muted)]" />
          <input
            value={search}
            onChange={e => setSearch(e.target.value)}
            placeholder={t('docs.searchPlaceholder')}
            className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md pl-9 pr-4 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors"
          />
        </div>
      </div>

      {/* Docs by category */}
      <div className="flex flex-col gap-8">
        {q
          ? (filtered.length ? (
              <div>
                <div className="flex items-center gap-2 mb-3">
                  <span className="text-base">🔍</span>
                  <h2 className="text-sm font-semibold uppercase tracking-wider text-[var(--text-muted)]">{t('docs.searchResults')}</h2>
                </div>
                <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
                  {filtered.map(doc => (
                    <DocCard key={doc.file} doc={doc} onOpen={open} title={docTitle(doc)} desc={docDesc(doc)} />
                  ))}
                </div>
              </div>
            ) : null)
          : CATEGORY_ORDER.map(cat => {
              const items = DOCS.filter(d => d.category === cat);
              if (!items.length) return null;
              return (
                <div key={cat}>
                  <div className="flex items-center gap-2 mb-3">
                    <span className="text-base">{CATEGORY_ICONS[cat]}</span>
                    <h2 className="text-sm font-semibold uppercase tracking-wider text-[var(--text-muted)]">{catTitle(cat)}</h2>
                  </div>
                  <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
                    {items.map(doc => (
                      <DocCard key={doc.file} doc={doc} onOpen={open} title={docTitle(doc)} desc={docDesc(doc)} />
                    ))}
                  </div>
                </div>
              );
            })}
      </div>
    </div>
  );
}

function DocCard({ doc, onOpen, title, desc }: { doc: DocEntry; onOpen: (d: DocEntry) => void; title: string; desc: string }) {
  return (
    <button onClick={() => onOpen(doc)}
      className="text-left p-4 bg-surface border border-[var(--border-subtle)] rounded-xl hover:border-[var(--accent)]/50 hover:bg-surface2 transition-all group">
      <div className="flex items-start justify-between gap-2 mb-1.5">
        <div className="flex items-center gap-2">
          <FileText size={14} className="text-[var(--accent)] flex-shrink-0 mt-0.5" />
          <span className="font-semibold text-sm text-[var(--text-primary)]">{title}</span>
        </div>
        <ChevronRight size={14} className="text-[var(--text-muted)] group-hover:text-[var(--accent)] transition-colors flex-shrink-0 mt-0.5" />
      </div>
      <p className="text-xs text-[var(--text-muted)] leading-relaxed ml-5">{desc}</p>
      <div className="ml-5 mt-2">
        <span className="text-[10px] font-mono text-[var(--text-muted)] opacity-50">{doc.file}</span>
      </div>
    </button>
  );
}
