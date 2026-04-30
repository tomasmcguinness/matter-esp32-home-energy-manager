import React, { createContext, useCallback, useContext, useEffect, useState } from 'react';
import {
  initialize,
  getCommissionedNodeIds,
  getHemNodeId,
  setHemNodeId as persistHemNodeId,
  getDeviceLabels,
  setDeviceLabel,
} from 'matter-commissioner';

export interface MatterDevice {
  nodeId: string;
  label: string;
}

type InitStatus = 'idle' | 'initializing' | 'ready' | 'error';

interface MatterContextType {
  initStatus: InitStatus;
  initError: string | null;
  controllerNodeId: string | null;
  hemNodeId: string | null;
  devices: MatterDevice[];
  setHemNodeId: (id: string) => void;
  addDevice: (nodeId: string, label: string) => void;
}

const MatterContext = createContext<MatterContextType>({
  initStatus: 'idle',
  initError: null,
  controllerNodeId: null,
  hemNodeId: null,
  devices: [],
  setHemNodeId: () => {},
  addDevice: () => {},
});

export function MatterProvider({ children }: { children: React.ReactNode }) {
  const [initStatus, setInitStatus] = useState<InitStatus>('idle');
  const [initError, setInitError] = useState<string | null>(null);
  const [controllerNodeId, setControllerNodeId] = useState<string | null>(null);
  const [hemNodeId, setHemNodeIdState] = useState<string | null>(null);
  const [devices, setDevices] = useState<MatterDevice[]>([]);

  useEffect(() => {
    let cancelled = false;

    async function init() {
      setInitStatus('initializing');
      try {
        const result = await initialize();
        if (cancelled) return;

        setControllerNodeId(result.nodeId);

        // Restore HEM node ID and device list from native UserDefaults
        const storedHemNodeId = getHemNodeId();
        setHemNodeIdState(storedHemNodeId);

        const labels = getDeviceLabels();
        const nodeIds = getCommissionedNodeIds();
        setDevices(
          nodeIds.map(id => ({
            nodeId: id,
            label: labels[id] ?? `Device …${id.slice(-4)}`,
          }))
        );

        setInitStatus('ready');
      } catch (e: any) {
        if (cancelled) return;
        setInitStatus('error');
        setInitError(e?.message ?? 'Failed to initialize Matter controller');
      }
    }

    init();
    return () => { cancelled = true; };
  }, []);

  const setHemNodeId = useCallback((id: string) => {
    setHemNodeIdState(id);
    persistHemNodeId(id);
  }, []);

  const addDevice = useCallback((nodeId: string, label: string) => {
    setDeviceLabel(nodeId, label);
    setDevices(prev => {
      if (prev.some(d => d.nodeId === nodeId)) return prev;
      return [...prev, { nodeId, label }];
    });
  }, []);

  return (
    <MatterContext.Provider
      value={{ initStatus, initError, controllerNodeId, hemNodeId, devices, setHemNodeId, addDevice }}
    >
      {children}
    </MatterContext.Provider>
  );
}

export const useMatter = () => useContext(MatterContext);
