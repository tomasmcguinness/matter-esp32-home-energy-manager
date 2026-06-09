import { NavLink, Routes, Route } from 'react-router'
import './App.css'
import Home from './Home.tsx'
import Topology from './Topology.tsx'
import Devices from './Devices.tsx'
import Power from './Power.tsx'
import Forecast from './Forecast.tsx'
import Settings from './Settings.tsx'
import Test from './Test.tsx'
import Appliances from './Appliances.tsx'

import '@xyflow/react/dist/style.css';

function App() {
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
              <li className="nav-item">
                <NavLink className="nav-link" to="/">Home</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/topology">Topology</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/appliances">Appliances</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/power">Power</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/forecast">Forecast</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/devices">Devices</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/settings">Settings</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/test">Test</NavLink>
              </li>
            </ul>
          </div>
        </div>
      </nav>
      <Routes>
        <Route path="/" element={<div className="container"><Home /></div>} />
        <Route path="/topology" element={<Topology />} />
        <Route path="/appliances" element={<div className="container"><Appliances /></div>} />
        <Route path="/power" element={<div className="container"><Power /></div>} />
        <Route path="/forecast" element={<div className="container"><Forecast /></div>} />
        <Route path="/devices" element={<div className="container"><Devices /></div>} />
        <Route path="/settings" element={<div className="container"><Settings /></div>} />
        <Route path="/test" element={<div className="container"><Test /></div>} />
      </Routes>
    </>
  )
}

export default App
