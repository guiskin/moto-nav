import SwiftUI

@main
struct MotoNavApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var location = LocationProvider()
    @StateObject private var engine = NavigationEngine()
    @StateObject private var places = PlacesStore()
    @StateObject private var settings = SettingsStore()
    @StateObject private var trips = TripStore()
    @StateObject private var playlists = PlaylistStore()
    @StateObject private var voiceCenter = VoiceCommandCenter()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
                .environmentObject(location)
                .environmentObject(engine)
                .environmentObject(places)
                .environmentObject(settings)
                .environmentObject(trips)
                .environmentObject(playlists)
                .environmentObject(voiceCenter)
                .onAppear {
                    engine.attach(location: location, ble: ble, settings: settings, trips: trips)
                    voiceCenter.attach(ble: ble, engine: engine, places: places, playlists: playlists)
                    location.start()
                }
        }
    }
}
