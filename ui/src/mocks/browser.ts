/**
 * MSW browser worker — dev-only.
 * Activated in main.tsx under import.meta.env.DEV.
 */
import { setupWorker } from 'msw/browser';
import { handlers } from './handlers';

export const worker = setupWorker(...handlers);
