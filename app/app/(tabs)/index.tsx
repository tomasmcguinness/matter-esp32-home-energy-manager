import { StyleSheet, View, Text, TouchableOpacity, ScrollView, Alert } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { router } from 'expo-router';

import { useMatter } from '@/context/matter-context';
import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';

function StatusBadge({ label, color }: { label: string; color: string }) {
  return (
    <View style={[styles.badge, { backgroundColor: color + '20', borderColor: color }]}>
      <View style={[styles.badgeDot, { backgroundColor: color }]} />
      <Text style={[styles.badgeText, { color }]}>{label}</Text>
    </View>
  );
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <View style={styles.infoRow}>
      <Text style={styles.infoLabel}>{label}</Text>
      <Text style={styles.infoValue} numberOfLines={1} ellipsizeMode="middle">
        {value}
      </Text>
    </View>
  );
}

export default function ControllerScreen() {
  const insets = useSafeAreaInsets();
  const { initStatus, initError, controllerNodeId, hemNodeId, devices, setHemNodeId } = useMatter();

  function handleRecommission() {
    Alert.alert(
      'Re-commission Home Energy Manager',
      'This will remove the current HEM pairing and start the onboarding process again.',
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Re-commission',
          style: 'destructive',
          onPress: () => {
            setHemNodeId('');
            router.replace('/onboard');
          },
        },
      ]
    );
  }

  return (
    <ScrollView
      style={styles.scroll}
      contentContainerStyle={[styles.content, { paddingBottom: insets.bottom + 24 }]}
    >
      {/* Controller Status Card */}
      <ThemedView style={styles.card}>
        <Text style={styles.cardTitle}>Matter Fabric</Text>
        <StatusBadge
          label={initStatus === 'ready' ? 'Active' : initStatus === 'error' ? 'Error' : 'Starting…'}
          color={initStatus === 'ready' ? '#16a34a' : initStatus === 'error' ? '#dc2626' : '#d97706'}
        />
        {initError && <Text style={styles.errorText}>{initError}</Text>}
        {controllerNodeId && (
          <InfoRow label="Controller Node ID" value={controllerNodeId} />
        )}
        <InfoRow label="Fabric ID" value="1" />
        <InfoRow label="Commissioned devices" value={String(devices.length)} />
      </ThemedView>

      {/* HEM Card */}
      <ThemedView style={styles.card}>
        <Text style={styles.cardTitle}>Home Energy Manager</Text>
        {hemNodeId ? (
          <>
            <StatusBadge label="Commissioned" color="#16a34a" />
            <InfoRow label="Node ID" value={hemNodeId} />
            <Text style={styles.cardCaption}>
              The HEM has been commissioned to this fabric. Any device added via the Devices tab will
              automatically grant the HEM operator-level access so it can read sensors and send commands.
            </Text>
            <TouchableOpacity style={styles.secondaryButton} onPress={handleRecommission}>
              <Text style={styles.secondaryButtonText}>Re-commission HEM…</Text>
            </TouchableOpacity>
          </>
        ) : (
          <>
            <StatusBadge label="Not commissioned" color="#9ca3af" />
            <Text style={styles.cardCaption}>
              The Home Energy Manager needs to be commissioned before you can add other devices.
            </Text>
            <TouchableOpacity
              style={styles.primaryButton}
              onPress={() => router.push('/onboard')}
            >
              <Text style={styles.primaryButtonText}>Commission HEM</Text>
            </TouchableOpacity>
          </>
        )}
      </ThemedView>

      {/* How It Works */}
      <ThemedView style={styles.card}>
        <Text style={styles.cardTitle}>How It Works</Text>
        <Text style={styles.cardCaption}>
          This app acts as a Matter commissioner. When you add a device, it is commissioned directly to this
          phone's Matter fabric using the iOS Matter SDK. The Home Energy Manager's node ID is then written
          to the device's Access Control List (ACL), giving the HEM permission to control it independently.
        </Text>
      </ThemedView>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: { flex: 1 },
  content: { padding: 16, gap: 12 },
  card: {
    borderRadius: 14,
    padding: 16,
    gap: 10,
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.06,
    shadowRadius: 4,
    elevation: 2,
  },
  cardTitle: { fontSize: 13, fontWeight: '600', color: '#6b7280', textTransform: 'uppercase', letterSpacing: 0.5 },
  cardCaption: { fontSize: 14, color: '#6b7280', lineHeight: 20 },
  badge: { flexDirection: 'row', alignItems: 'center', gap: 6, alignSelf: 'flex-start', borderRadius: 20, borderWidth: 1, paddingHorizontal: 10, paddingVertical: 4 },
  badgeDot: { width: 7, height: 7, borderRadius: 4 },
  badgeText: { fontSize: 13, fontWeight: '600' },
  infoRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', gap: 8 },
  infoLabel: { fontSize: 14, color: '#6b7280', flexShrink: 0 },
  infoValue: { fontSize: 14, fontWeight: '500', color: '#11181C', flexShrink: 1, textAlign: 'right' },
  errorText: { fontSize: 13, color: '#dc2626' },
  primaryButton: { backgroundColor: '#0a7ea4', borderRadius: 10, paddingVertical: 12, alignItems: 'center' },
  primaryButtonText: { color: '#fff', fontWeight: '600', fontSize: 15 },
  secondaryButton: { borderWidth: 1, borderColor: '#e5e7eb', borderRadius: 10, paddingVertical: 10, alignItems: 'center' },
  secondaryButtonText: { color: '#dc2626', fontWeight: '500', fontSize: 14 },
});
