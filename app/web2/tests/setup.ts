import '@testing-library/jest-dom/vitest';

// Ensure jsdom always has a <html> element for theme-attribute tests.
// jsdom provides this by default but the tests are explicit about reading it.
