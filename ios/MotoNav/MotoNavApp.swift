import SwiftUI

@main
struct MotoNavApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var location = LocationProvider()
    @StateObject private var engine = NavigationEngine()
    @StateObject private var places = PlacesStore()
    @StateObject private var settings = SettingsStore()
    @StateObject private var trips = TripStore()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
                .environmentObject(location)
                .environmentObject(engine)
                .environmentObject(places)
                .environmentObject(settings)
                .environmentObject(trips)
                .onAppear {
                    engine.attach(location: location, ble: ble, settings: settings, trips: trips)
                    location.start()
                }
        }
    }
}
