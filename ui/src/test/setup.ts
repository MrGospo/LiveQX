import '@testing-library/jest-dom';

// Initialize a real i18next instance for the test environment. The app boots
// i18n from src/main.tsx (which relies on browser-only LanguageDetector), but
// jsdom has no localStorage-driven detection to lean on and the individual
// test files should not each mock react-i18next by hand. Here we register the
// EN bundle synchronously so `t('some.key')` returns real English strings.
import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import enJson from '@/locales/en.json';

if (!i18n.isInitialized) {
  i18n.use(initReactI18next).init({
    resources: { en: { translation: enJson } },
    lng: 'en',
    fallbackLng: 'en',
    interpolation: { escapeValue: false },
  });
}
