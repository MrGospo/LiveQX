import type { Config } from 'tailwindcss';

const config: Config = {
  darkMode: 'class',
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        // Design tokens from tokens.css mapped to Tailwind
        canvas:    'var(--bg-canvas)',
        surface:   'var(--bg-surface)',
        surface2:  'var(--bg-surface-2)',
        border:    'var(--border-subtle)',
        primary:   'var(--text-primary)',
        muted:     'var(--text-muted)',
        accent:    'var(--accent)',
        'accent-h':'var(--accent-hover)',
        success:   'var(--success)',
        warning:   'var(--warning)',
        danger:    'var(--danger)',
        info:      'var(--info)',
      },
      borderRadius: {
        sm:  '4px',
        md:  '6px',
        lg:  '8px',
        xl:  '12px',
        '2xl': '16px',
      },
      fontFamily: {
        sans: ['Inter', 'IBM Plex Sans', 'system-ui', '-apple-system', 'sans-serif'],
        mono: ['IBM Plex Mono', 'Fira Code', 'monospace'],
      },
      fontSize: {
        xs:   ['12px', { lineHeight: '1.5' }],
        sm:   ['14px', { lineHeight: '1.5' }],
        base: ['15px', { lineHeight: '1.5' }],
        lg:   ['17px', { lineHeight: '1.4' }],
        xl:   ['20px', { lineHeight: '1.3' }],
        '2xl':['24px', { lineHeight: '1.2' }],
        '3xl':['30px', { lineHeight: '1.2' }],
        '4xl':['36px', { lineHeight: '1.1' }],
      },
      spacing: {
        sidebar: 'var(--sidebar-w)',
        topbar:  'var(--topbar-h)',
      },
      ringColor: { DEFAULT: 'var(--accent)' },
      ringOffsetColor: { DEFAULT: 'var(--bg-canvas)' },
    },
  },
  plugins: [],
};

export default config;
