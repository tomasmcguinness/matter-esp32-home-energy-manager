import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  build: {
    rollupOptions: {
      output: {
        dir: '../html_compiled_app',
        entryFileNames: 'app.js',
        assetFileNames: 'app.css',
        chunkFileNames: 'chunk.js',
        manualChunks: undefined,
      }
    }
  }
})
