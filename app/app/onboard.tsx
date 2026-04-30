import { CameraView, useCameraPermissions } from 'expo-camera';
import { router } from 'expo-router';
import { useState } from 'react';
import {
  ActivityIndicator,
  Alert,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { useMatter } from '@/context/matter-context';
import { commissionDevice } from 'matter-commissioner';

type Stage = 'scan' | 'manual' | 'commissioning' | 'done';

export default function OnboardScreen() {
  const insets = useSafeAreaInsets();
  const { setHemNodeId, initStatus } = useMatter();
  const [permission, requestPermission] = useCameraPermissions();
  const [stage, setStage] = useState<Stage>('scan');
  const [manualCode, setManualCode] = useState('');
  const [statusMessage, setStatusMessage] = useState('');
  const [scanned, setScanned] = useState(false);

  async function commission(payload: string) {
    setStage('commissioning');
    setStatusMessage('Establishing secure session with the Home Energy Manager…');
    try {
      const result = await commissionDevice(payload);
      setStatusMessage('Commissioning complete!');
      setHemNodeId(result.nodeId);
      setStage('done');
      setTimeout(() => router.replace('/(tabs)'), 1200);
    } catch (e: any) {
      Alert.alert(
        'Commissioning Failed',
        e.message ?? 'Could not commission the Home Energy Manager. Make sure it is in commissioning mode and on the same network.',
        [{ text: 'Try Again', onPress: () => { setScanned(false); setStage('scan'); } }]
      );
    }
  }

  if (initStatus === 'initializing' || initStatus === 'idle') {
    return (
      <View style={[styles.center, { paddingTop: insets.top }]}>
        <ActivityIndicator size="large" color="#0a7ea4" />
        <Text style={styles.statusText}>Starting Matter controller…</Text>
      </View>
    );
  }

  if (initStatus === 'error') {
    return (
      <View style={[styles.center, { paddingTop: insets.top }]}>
        <Text style={styles.errorText}>Matter controller failed to start.</Text>
        <Text style={styles.body}>This app requires iOS 16.4 or later.</Text>
      </View>
    );
  }

  if (stage === 'commissioning' || stage === 'done') {
    return (
      <View style={[styles.center, { paddingTop: insets.top }]}>
        {stage === 'commissioning' ? (
          <ActivityIndicator size="large" color="#0a7ea4" />
        ) : (
          <Text style={styles.successIcon}>✓</Text>
        )}
        <Text style={styles.statusText}>{statusMessage}</Text>
      </View>
    );
  }

  if (stage === 'manual') {
    return (
      <View style={[styles.center, styles.padded, { paddingTop: insets.top + 32 }]}>
        <Text style={styles.title}>Enter Pairing Code</Text>
        <Text style={styles.body}>
          Enter the 11-digit Matter pairing code printed on your Home Energy Manager.
        </Text>
        <TextInput
          style={styles.codeInput}
          value={manualCode}
          onChangeText={setManualCode}
          keyboardType="number-pad"
          placeholder="00000-00000-0"
          maxLength={13}
          autoFocus
        />
        <TouchableOpacity
          style={[styles.button, !manualCode && styles.buttonDisabled]}
          disabled={!manualCode}
          onPress={() => commission(manualCode.replace(/-/g, ''))}
        >
          <Text style={styles.buttonLabel}>Commission</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.link} onPress={() => setStage('scan')}>
          <Text style={styles.linkText}>Scan QR code instead</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // QR scan stage
  if (!permission) {
    return <View style={styles.center} />;
  }

  if (!permission.granted) {
    return (
      <View style={[styles.center, styles.padded, { paddingTop: insets.top + 32 }]}>
        <Text style={styles.title}>Camera Permission Required</Text>
        <Text style={styles.body}>
          The camera is needed to scan the Matter QR code on your Home Energy Manager.
        </Text>
        <TouchableOpacity style={styles.button} onPress={requestPermission}>
          <Text style={styles.buttonLabel}>Grant Camera Access</Text>
        </TouchableOpacity>
      </View>
    );
  }

  return (
    <View style={styles.full}>
      <CameraView
        style={styles.full}
        facing="back"
        barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
        onBarcodeScanned={scanned ? undefined : ({ data }) => {
          if (!data.startsWith('MT:')) return;
          setScanned(true);
          commission(data);
        }}
      />
      <View style={[styles.overlay, { paddingTop: insets.top + 16, paddingBottom: insets.bottom + 16 }]}>
        <Text style={styles.overlayTitle}>Commission Home Energy Manager</Text>
        <Text style={styles.overlayBody}>
          Point the camera at the QR code on your ESP32 Home Energy Manager.
        </Text>
        <View style={styles.viewfinder} />
        <TouchableOpacity style={styles.manualButton} onPress={() => setStage('manual')}>
          <Text style={styles.manualButtonText}>Enter code manually</Text>
        </TouchableOpacity>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  full: { flex: 1 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: '#fff' },
  padded: { paddingHorizontal: 32 },
  title: { fontSize: 24, fontWeight: '700', marginBottom: 12, textAlign: 'center' },
  body: { fontSize: 15, color: '#444', textAlign: 'center', lineHeight: 22, marginBottom: 24 },
  statusText: { marginTop: 16, fontSize: 16, color: '#444', textAlign: 'center', paddingHorizontal: 24 },
  errorText: { fontSize: 18, fontWeight: '600', color: '#c00', marginBottom: 8 },
  successIcon: { fontSize: 56, color: '#16a34a' },
  codeInput: {
    borderWidth: 1.5,
    borderColor: '#0a7ea4',
    borderRadius: 10,
    fontSize: 20,
    padding: 14,
    width: '100%',
    textAlign: 'center',
    letterSpacing: 2,
    marginBottom: 20,
  },
  button: {
    backgroundColor: '#0a7ea4',
    borderRadius: 12,
    paddingVertical: 14,
    paddingHorizontal: 32,
    alignItems: 'center',
    width: '100%',
    marginBottom: 12,
  },
  buttonDisabled: { opacity: 0.4 },
  buttonLabel: { color: '#fff', fontSize: 16, fontWeight: '600' },
  link: { paddingVertical: 8 },
  linkText: { color: '#0a7ea4', fontSize: 15 },
  overlay: {
    ...StyleSheet.absoluteFillObject,
    alignItems: 'center',
    paddingHorizontal: 24,
  },
  overlayTitle: {
    color: '#fff',
    fontSize: 20,
    fontWeight: '700',
    textShadowColor: 'rgba(0,0,0,0.8)',
    textShadowOffset: { width: 0, height: 1 },
    textShadowRadius: 4,
    textAlign: 'center',
    marginBottom: 8,
  },
  overlayBody: {
    color: '#fff',
    fontSize: 14,
    textShadowColor: 'rgba(0,0,0,0.8)',
    textShadowOffset: { width: 0, height: 1 },
    textShadowRadius: 4,
    textAlign: 'center',
    marginBottom: 24,
  },
  viewfinder: {
    flex: 1,
    width: 260,
    borderWidth: 2,
    borderColor: '#0a7ea4',
    borderRadius: 16,
    maxHeight: 260,
  },
  manualButton: {
    marginTop: 24,
    backgroundColor: 'rgba(0,0,0,0.55)',
    borderRadius: 20,
    paddingVertical: 10,
    paddingHorizontal: 24,
  },
  manualButtonText: { color: '#fff', fontSize: 14, fontWeight: '500' },
});
