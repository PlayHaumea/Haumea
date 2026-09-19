import SwiftUI

struct SettingsView: View {
    @AppStorage("networkEnabled") private var networkEnabled = true
    @AppStorage("spatialFX") private var spatialFX = false
    @AppStorage("volume") private var volume: Double = 100.0
    
    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Graphics")) {
                    Toggle("MetalFX Spatial Upscaling", isOn: $spatialFX)
                        .onChange(of: spatialFX) { newValue in
                            haumea_set_metal_fx(newValue, false, false)
                        }
                }
                
                Section(header: Text("Audio")) {
                    Slider(value: $volume, in: 0...100, step: 1.0) {
                        Text("Volume")
                    }
                    .onChange(of: volume) { newValue in
                        haumea_set_volume(Int32(newValue))
                    }
                }
                
                Section(header: Text("Network")) {
                    Toggle("Enable Networking", isOn: $networkEnabled)
                        .onChange(of: networkEnabled) { newValue in
                            haumea_set_network_enabled(newValue)
                        }
                }
            }
            .navigationTitle("Settings")
        }
    }
}
