//
//  ContentView.swift
//  home-energy-manager-commissioner
//
//  Created by Tomas McGuinness on 30/04/2026.
//

import SwiftUI
import Matter
import MatterSupport
import os.log

struct ContentView: View {
    var body: some View {
        VStack {
            Button("Connect to Hub") {
                Task {
                    await commissionHub()
                }
            }
        }
        .padding()
    }
    
    private func commissionHub() async {
        print("Commissioning hub...")
        
        let homes = [MatterAddDeviceRequest.Home(displayName: "My Home")]
        let topology = MatterAddDeviceRequest.Topology(ecosystemName: "Home Energy Manager", homes: homes)
        
        let request = MatterAddDeviceRequest(topology: topology)
        
        do {
            try await request.perform()
            print("Successfully set up device!")
        } catch {
            print("Failed to set up device with error: \(error.localizedDescription).")
        }
    }
}

#Preview {
    ContentView()
}
