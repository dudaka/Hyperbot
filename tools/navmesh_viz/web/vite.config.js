import { defineConfig } from 'vite';

// Proxy backend endpoints to the navmesh_viz C++ service so the client can use
// same-origin relative URLs (/geometry, /path, /info).
export default defineConfig({
  server: {
    proxy: {
      '/geometry': 'http://127.0.0.1:5577',
      '/path': 'http://127.0.0.1:5577',
      '/info': 'http://127.0.0.1:5577',
    },
  },
});
