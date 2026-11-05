import SwiftUI

@main
struct MotoNavApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var location = LocationProvider()
    @StateObject private var engine = NavigationEngine()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
                .environmentObject(location)
                .environmentObject(engine)
                .onAppear {
                    engine.attach(location: location, ble: ble)
                    location.start()
                }
        }
    }
}
