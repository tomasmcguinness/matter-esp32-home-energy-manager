import type { ReactNode } from 'react'
import { NavLink, Routes, Route, Link } from 'react-router'
import './App.css'
import Home from './Home.tsx'
import Topology from './Topology.tsx'
import Devices from './Devices.tsx'
import CommissioningWindow from './CommissioningWindow.tsx'
import Power from './Power.tsx'
import Forecast from './Forecast.tsx'
import Schedule from './Schedule.tsx'
import Settings from './Settings.tsx'
import Test from './Test.tsx'
import Appliances from './Appliances.tsx'
import SetupProvider from './SetupProvider.tsx'
import { useSetup, type Setup } from './setup.ts'

import '@xyflow/react/dist/style.css';

// Every page, in nav order. A page with `visible` appears once the home is set
// up enough for it to show something; `needs` says what unlocks it.
type Page = {
  path: string
  label?: string // omitted for routes that are not in the nav
  element: ReactNode
  container?: boolean
  visible?: (s: Setup) => boolean
  needs?: ReactNode
}

const PAGES: Page[] = [
  { path: '/', label: 'Home', element: <Home /> },
  { path: '/topology', label: 'Topology', element: <Topology />, container: false },
  {
    path: '/appliances', label: 'Appliances', element: <Appliances />, visible: s => s.hasLoads,
    needs: <>Assign a smart plug or clamp to one of the <Link to="/">Loads</Link> to see its learned profile.</>,
  },
  {
    path: '/power', label: 'Power', element: <Power />, visible: s => s.hasMeters,
    needs: <>Assign a <Link to="/">grid sensor or a load</Link> to start recording power.</>,
  },
  {
    path: '/forecast', label: 'Forecast', element: <Forecast />, visible: s => s.hasSolarAndGrid,
    needs: <>Forecasting needs a <Link to="/">grid sensor and a solar inverter</Link>.</>,
  },
  {
    path: '/schedule', label: 'Schedule', element: <Schedule />, visible: s => s.hasSolarAndGrid,
    needs: <>Scheduling needs a <Link to="/">grid sensor and a solar inverter</Link>.</>,
  },
  {
    path: '/devices', label: 'Devices', element: <Devices />, visible: s => s.hasDevices,
    needs: <>No Matter devices have been commissioned yet.</>,
  },
  { path: '/devices/:nodeId/commissioning', element: <CommissioningWindow /> },
  { path: '/settings', label: 'Settings', element: <Settings /> },
  {
    path: '/test', label: 'Test', element: <Test />, visible: s => s.advanced,
    needs: <>Turn on <Link to="/settings">advanced features</Link> to use the test tools.</>,
  },
]

// Shown at a page's URL while it is still hidden, so a bookmark never lands on a blank page.
function NotYet({ label, needs }: { label?: string; needs?: ReactNode }) {
  return (
    <div className="mt-4" style={{ border: '1px solid #e2e8f0', borderRadius: 8, padding: '16px', background: '#fff' }}>
      <div style={{ fontWeight: 600, color: '#1e293b' }}>{label ?? 'This page'} isn't available yet</div>
      <div style={{ fontSize: 13, color: '#64748b', marginTop: 4 }}>{needs}</div>
    </div>
  )
}

function AppShell() {
  const setup = useSetup()
  // Every has* flag is false until the configuration loads, so gated tabs stay
  // hidden rather than flashing up and then disappearing.
  const isVisible = (p: Page) => !p.visible || p.visible(setup)

  return (
    <>
      <nav className="navbar navbar-expand-lg">
        <div className="container">
          <a className="navbar-brand" href="#">Home Energy Manager</a>
          <button className="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarNav" aria-controls="navbarNav" aria-expanded="false" aria-label="Toggle navigation">
            <span className="navbar-toggler-icon"></span>
          </button>
          <div className="collapse navbar-collapse" id="navbarNav">
            <ul className="navbar-nav">
              {PAGES.filter(p => p.label && isVisible(p)).map(p => (
                <li key={p.path} className="nav-item">
                  <NavLink className="nav-link" to={p.path}>{p.label}</NavLink>
                </li>
              ))}
            </ul>
          </div>
        </div>
      </nav>
      <Routes>
        {PAGES.map(p => {
          const body = isVisible(p) ? p.element
            : setup.loaded ? <NotYet label={p.label} needs={p.needs} />
              : null // still loading: decide once we know what's configured
          return (
            <Route key={p.path} path={p.path}
              element={p.container === false && isVisible(p) ? body : <div className="container">{body}</div>} />
          )
        })}
      </Routes>
    </>
  )
}

function App() {
  return (
    <SetupProvider>
      <AppShell />
    </SetupProvider>
  )
}

export default App
