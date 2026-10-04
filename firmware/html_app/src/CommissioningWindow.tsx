import { useEffect, useState } from "react";
import { QRCodeSVG } from "qrcode.react";
import { NavLink, useLocation, useParams } from "react-router"

interface CommissioningWindowState {
  manualCode: string;
  qrCode: string;
  timeout: number;   // seconds
  openedAt: number;  // ms since epoch, stamped by the Devices page
}

// 11-digit codes are shown as XXXX-XXX-XXXX, which is how Matter apps print them.
function formatManualCode(code: string): string {
  if (code.length === 11) {
    return `${code.slice(0, 4)}-${code.slice(4, 7)}-${code.slice(7)}`;
  }
  return code;
}

function formatRemaining(seconds: number): string {
  const minutes = Math.floor(seconds / 60);
  return `${minutes}:${(seconds % 60).toString().padStart(2, '0')}`;
}

function CommissioningWindow() {

  const { nodeId } = useParams();

  const state = useLocation().state as CommissioningWindowState | null;

  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, []);

  const heading = <h1 className="mt-3">Device - 0x{BigInt(nodeId ?? 0).toString(16).toUpperCase()} - Commissioning</h1>;

  if (!state?.manualCode) {
    return (
      <>
        {heading}
        <hr />
        <p>No commissioning window has been opened. Open one from the Devices page.</p>
        <NavLink className="btn btn-default" to="/devices">Back</NavLink>
      </>
    )
  }

  const remaining = Math.max(0, Math.round((state.openedAt + state.timeout * 1000 - now) / 1000));
  const expired = remaining === 0;

  return (
    <>
      {heading}
      <hr />
      <p>
        Scan the QR code, or enter the setup code, in the other Matter controller's app to add this device to it.
      </p>

      {expired
        ? <div className="alert alert-warning" role="alert">This commissioning window has closed. Open a new one from the Devices page.</div>
        : <p>The window closes in <strong>{formatRemaining(remaining)}</strong>.</p>}

      <div style={{ opacity: expired ? 0.3 : 1 }}>
        <div style={{ background: '#ffffff', display: 'inline-block', padding: '16px', marginBottom: '20px' }}>
          <QRCodeSVG value={state.qrCode} size={256} level="M" />
        </div>

        <table className="table table-bordered" style={{ maxWidth: '520px' }}>
          <tbody>
            <tr>
              <th style={{ width: '140px' }}>Setup code</th>
              <td style={{ fontFamily: 'monospace', fontSize: '1.5em' }}>{formatManualCode(state.manualCode)}</td>
            </tr>
            <tr>
              <th>QR payload</th>
              <td style={{ fontFamily: 'monospace' }}>{state.qrCode}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <NavLink className="btn btn-default" to="/devices">Back</NavLink>
    </>
  )
}

export default CommissioningWindow
