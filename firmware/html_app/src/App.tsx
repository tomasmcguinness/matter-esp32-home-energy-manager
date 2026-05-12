import { NavLink, Routes, Route } from 'react-router'
import './App.css'
import Home from './Home.tsx'
import Devices from './Devices.tsx'
import Settings from './Settings.tsx'

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
                <NavLink className="nav-link" to="/devices">Devices</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/settings">Settings</NavLink>
              </li>
            </ul>
          </div>
        </div>
      </nav>
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/devices" element={<div className="container"><Devices /></div>} />
        <Route path="/settings" element={<div className="container"><Settings /></div>} />
      </Routes>
    </>
  )
}

export default App
