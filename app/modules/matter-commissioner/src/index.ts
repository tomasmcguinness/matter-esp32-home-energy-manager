import { requireNativeModule } from 'expo-modules-core';

export interface CommissionResult {
  nodeId: string;  // UInt64 as string — too large for JS number
}

export interface ACLEntry {
  privilege: number;
  authMode: number;
  subjects: string[];
}

export interface InitializeResult {
  fabricId: string;
  nodeId: string;  // controller's own node ID
}

const MatterCommissioner = requireNativeModule('MatterCommissioner');

/**
 * Initialize the Matter fabric and device controller.
 * Must be called once before any other function.
 * Safe to call multiple times — idempotent.
 */
export function initialize(): Promise<InitializeResult> {
  return MatterCommissioner.initialize();
}

/**
 * Commission a device from its QR code string (starts with "MT:")
 * or an 11-digit manual pairing code.
 * Returns the new device's node ID on our fabric.
 */
export function commissionDevice(setupPayload: string): Promise<CommissionResult> {
  return MatterCommissioner.commissionDevice(setupPayload);
}

/**
 * Grant a controller node operator-level access to a commissioned device
 * by writing an ACL entry on the device's Access Control cluster.
 * Call this after commissioning a device to give the HEM control over it.
 */
export function grantControllerAccess(
  deviceNodeId: string,
  controllerNodeId: string
): Promise<void> {
  return MatterCommissioner.grantControllerAccess(deviceNodeId, controllerNodeId);
}

/** Returns all node IDs currently tracked by the controller fabric. */
export function getCommissionedNodeIds(): string[] {
  return MatterCommissioner.getCommissionedNodeIds();
}

/** Persist the HEM's node ID so it survives app restarts. */
export function setHemNodeId(nodeId: string): void {
  MatterCommissioner.setHemNodeId(nodeId);
}

/** Retrieve the stored HEM node ID, or null if the HEM hasn't been commissioned yet. */
export function getHemNodeId(): string | null {
  return MatterCommissioner.getHemNodeId() ?? null;
}

/** Persist a human-readable label for a commissioned device node ID. */
export function setDeviceLabel(nodeId: string, label: string): void {
  MatterCommissioner.setDeviceLabel(nodeId, label);
}

/** Retrieve all stored device labels keyed by node ID. */
export function getDeviceLabels(): Record<string, string> {
  return MatterCommissioner.getDeviceLabels();
}
