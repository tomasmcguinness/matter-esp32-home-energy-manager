import { FlatList, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { router } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { useMatter, type MatterDevice } from '@/context/matter-context';
import { ThemedView } from '@/components/themed-view';

function DeviceRow({ device }: { device: MatterDevice }) {
  return (
    <View style={styles.row}>
      <View style={styles.rowIcon}>
        <Text style={styles.rowIconText}>⚡</Text>
      </View>
      <View style={styles.rowContent}>
        <Text style={styles.rowLabel}>{device.label}</Text>
        <Text style={styles.rowNodeId} numberOfLines={1} ellipsizeMode="middle">
          Node {device.nodeId}
        </Text>
      </View>
      <View style={styles.rowBadge}>
        <Text style={styles.rowBadgeText}>Commissioned</Text>
      </View>
    </View>
  );
}

function EmptyState({ hemCommissioned }: { hemCommissioned: boolean }) {
  return (
    <View style={styles.empty}>
      <Text style={styles.emptyIcon}>📡</Text>
      <Text style={styles.emptyTitle}>No devices yet</Text>
      <Text style={styles.emptyBody}>
        {hemCommissioned
          ? 'Tap "Add Device" to scan a Matter device QR code and commission it to this fabric. The Home Energy Manager will automatically be granted access.'
          : 'Commission the Home Energy Manager first (Controller tab), then add devices here.'}
      </Text>
    </View>
  );
}

export default function DevicesScreen() {
  const insets = useSafeAreaInsets();
  const { devices, hemNodeId, initStatus } = useMatter();
  const canAdd = initStatus === 'ready' && !!hemNodeId;

  return (
    <ThemedView style={styles.container}>
      <FlatList
        data={devices}
        keyExtractor={d => d.nodeId}
        renderItem={({ item }) => <DeviceRow device={item} />}
        contentContainerStyle={[
          styles.list,
          { paddingBottom: insets.bottom + 80 },
          devices.length === 0 && styles.listEmpty,
        ]}
        ListEmptyComponent={<EmptyState hemCommissioned={!!hemNodeId} />}
        ItemSeparatorComponent={() => <View style={styles.separator} />}
      />

      <View style={[styles.fab, { bottom: insets.bottom + 20 }]}>
        <TouchableOpacity
          style={[styles.fabButton, !canAdd && styles.fabButtonDisabled]}
          disabled={!canAdd}
          onPress={() => router.push('/add-device')}
        >
          <Text style={styles.fabIcon}>+</Text>
          <Text style={styles.fabLabel}>Add Device</Text>
        </TouchableOpacity>
      </View>
    </ThemedView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  list: { padding: 16 },
  listEmpty: { flex: 1, justifyContent: 'center' },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: '#fff',
    borderRadius: 12,
    padding: 14,
    gap: 12,
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.06,
    shadowRadius: 3,
    elevation: 2,
  },
  rowIcon: { width: 40, height: 40, borderRadius: 10, backgroundColor: '#f0f9ff', alignItems: 'center', justifyContent: 'center' },
  rowIconText: { fontSize: 20 },
  rowContent: { flex: 1 },
  rowLabel: { fontSize: 15, fontWeight: '600', color: '#11181C' },
  rowNodeId: { fontSize: 12, color: '#9ca3af', marginTop: 2 },
  rowBadge: { backgroundColor: '#dcfce7', borderRadius: 8, paddingHorizontal: 8, paddingVertical: 4 },
  rowBadgeText: { fontSize: 11, color: '#16a34a', fontWeight: '600' },
  separator: { height: 8 },
  empty: { alignItems: 'center', paddingHorizontal: 40, paddingVertical: 32 },
  emptyIcon: { fontSize: 48, marginBottom: 16 },
  emptyTitle: { fontSize: 20, fontWeight: '700', color: '#11181C', marginBottom: 10 },
  emptyBody: { fontSize: 15, color: '#6b7280', textAlign: 'center', lineHeight: 22 },
  fab: { position: 'absolute', left: 16, right: 16 },
  fabButton: {
    flexDirection: 'row',
    backgroundColor: '#0a7ea4',
    borderRadius: 14,
    paddingVertical: 14,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 8,
    shadowColor: '#0a7ea4',
    shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.35,
    shadowRadius: 8,
    elevation: 6,
  },
  fabButtonDisabled: { backgroundColor: '#94a3b8', shadowOpacity: 0 },
  fabIcon: { color: '#fff', fontSize: 22, fontWeight: '300' },
  fabLabel: { color: '#fff', fontSize: 16, fontWeight: '600' },
});
