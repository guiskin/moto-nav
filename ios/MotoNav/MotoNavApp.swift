import SwiftUI

@main
struct MotoNavApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var location = LocationProvider()
    @StateObject private var engine = NavigationEngine()
    @StateObject private var places = PlacesStore()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
                .environmentObject(location)
                .environmentObject(engine)
                .environmentObject(places)
                .onAppear {
                    engine.attach(location: location, ble: ble)
                    location.start()
                }
        }
    }
}
