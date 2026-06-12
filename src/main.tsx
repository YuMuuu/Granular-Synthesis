import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import App from './App'
import './index.css'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)

// Request initial processor state
if (typeof globalThis.__postNativeMessage__ === 'function') {
  globalThis.__postNativeMessage__("ready");
}
