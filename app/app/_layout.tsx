import { DarkTheme, DefaultTheme, ThemeProvider } from '@react-navigation/native';
import { Stack, router } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { useEffect } from 'react';
import 'react-native-reanimated';

import { MatterProvider, useMatter } from '@/context/matter-context';
import { useColorScheme } from '@/hooks/use-color-scheme';

export const unstable_settings = {
  anchor: '(tabs)',
};

function NavigationRouter() {
  const { initStatus, hemNodeId } = useMatter();

  useEffect(() => {
    if (initStatus !== 'ready') return;
    if (!hemNodeId) {
      // HEM not yet commissioned — send user to onboarding
      router.replace('/onboard');
    }
  }, [initStatus, hemNodeId]);

  return null;
}

export default function RootLayout() {
  const colorScheme = useColorScheme();

  return (
    <ThemeProvider value={colorScheme === 'dark' ? DarkTheme : DefaultTheme}>
      <MatterProvider>
        <NavigationRouter />
        <Stack>
          <Stack.Screen name="(tabs)" options={{ headerShown: false }} />
          <Stack.Screen
            name="onboard"
            options={{ headerShown: false, gestureEnabled: false }}
          />
          <Stack.Screen
            name="add-device"
            options={{
              presentation: 'modal',
              title: 'Add Device',
              headerBackTitle: 'Cancel',
            }}
          />
        </Stack>
        <StatusBar style="auto" />
      </MatterProvider>
    </ThemeProvider>
  );
}
