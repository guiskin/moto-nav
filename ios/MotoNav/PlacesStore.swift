import Foundation
import MapKit

struct SavedPlace: Codable, Identifiable, Equatable {
    var id: String { String(format: "%.5f,%.5f", latitude, longitude) }
    var name: String
    var subtitle: String
    var latitude: Double
    var longitude: Double
    var date: Date

    init(name: String, subtitle: String, coordinate: CLLocationCoordinate2D) {
        self.name = name
        self.subtitle = subtitle
        latitude = coordinate.latitude
        longitude = coordinate.longitude
        date = Date()
    }

    init(item: MKMapItem, name customName: String? = nil) {
        name = customName ?? item.name ?? "Destino"
        let pm = item.placemark
        let street = [pm.thoroughfare, pm.subThoroughfare].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: ", ")
        subtitle = ([street, pm.subLocality ?? "", pm.locality ?? "", pm.administrativeArea ?? ""])
            .filter { !$0.isEmpty && $0 != item.name }
            .joined(separator: " - ")
        latitude = pm.coordinate.latitude
        longitude = pm.coordinate.longitude
        date = Date()
    }

    var mapItem: MKMapItem {
        let item = MKMapItem(placemark: MKPlacemark(coordinate: CLLocationCoordinate2D(latitude: latitude, longitude: longitude)))
        item.name = name
        return item
    }

    static func == (a: SavedPlace, b: SavedPlace) -> Bool { a.id == b.id }
}

final class PlacesStore: ObservableObject {
    @Published private(set) var favorites: [SavedPlace] = []
    @Published private(set) var recents: [SavedPlace] = []

    private let favKey = "motonav.favorites"
    private let recKey = "motonav.recents"

    init() {
        favorites = load(favKey)
        recents = load(recKey)
    }

    func isFavorite(_ item: MKMapItem?) -> Bool {
        guard let item = item else { return false }
        let p = SavedPlace(item: item)
        return favorites.contains(p)
    }

    func toggleFavorite(_ item: MKMapItem?, name: String? = nil) {
        guard let item = item else { return }
        let clean = name?.trimmingCharacters(in: .whitespaces)
        let p = SavedPlace(item: item, name: (clean?.isEmpty ?? true) ? nil : clean)
        if let i = favorites.firstIndex(of: p) {
            favorites.remove(at: i)
        } else {
            favorites.insert(p, at: 0)
        }
        save(favorites, favKey)
    }

    func addFavorite(_ item: MKMapItem, name: String) {
        let clean = name.trimmingCharacters(in: .whitespaces)
        let p = SavedPlace(item: item, name: clean.isEmpty ? nil : clean)
        favorites.removeAll { $0 == p }
        favorites.insert(p, at: 0)
        save(favorites, favKey)
    }

    func saveFavorite(replacing old: SavedPlace?, name: String, subtitle: String, coordinate: CLLocationCoordinate2D) {
        let clean = name.trimmingCharacters(in: .whitespaces)
        guard !clean.isEmpty else { return }
        let p = SavedPlace(name: clean, subtitle: subtitle, coordinate: coordinate)
        if let old = old, let i = favorites.firstIndex(where: { $0.id == old.id }) {
            favorites[i] = p
        } else {
            favorites.removeAll { $0 == p }
            favorites.insert(p, at: 0)
        }
        save(favorites, favKey)
    }

    func renameFavorite(_ p: SavedPlace, to name: String) {
        let clean = name.trimmingCharacters(in: .whitespaces)
        guard !clean.isEmpty, let i = favorites.firstIndex(of: p) else { return }
        favorites[i].name = clean
        save(favorites, favKey)
    }

    func removeFavorite(_ p: SavedPlace) {
        favorites.removeAll { $0 == p }
        save(favorites, favKey)
    }

    func addRecent(_ item: MKMapItem) {
        let p = SavedPlace(item: item)
        recents.removeAll { $0 == p }
        recents.insert(p, at: 0)
        if recents.count > 20 { recents = Array(recents.prefix(20)) }
        save(recents, recKey)
    }

    func removeRecent(_ p: SavedPlace) {
        recents.removeAll { $0 == p }
        save(recents, recKey)
    }

    func clearRecents() {
        recents = []
        save(recents, recKey)
    }

    private func load(_ key: String) -> [SavedPlace] {
        guard let data = UserDefaults.standard.data(forKey: key),
              let list = try? JSONDecoder().decode([SavedPlace].self, from: data) else { return [] }
        return list
    }

    private func save(_ list: [SavedPlace], _ key: String) {
        if let data = try? JSONEncoder().encode(list) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }
}
