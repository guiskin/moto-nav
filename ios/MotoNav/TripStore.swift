import Foundation
import CoreLocation

struct Trip: Codable, Identifiable {
    let id: UUID
    let title: String
    let start: Date
    let end: Date
    let distance: Double
    let movingTime: TimeInterval
    let maxSpeed: Double
    let points: [[Double]]

    var avgSpeed: Double { movingTime > 0 ? distance / movingTime : 0 }
    var coordinates: [CLLocationCoordinate2D] { points.map { CLLocationCoordinate2D(latitude: $0[0], longitude: $0[1]) } }
}

final class TripRecorder {
    private(set) var title: String
    private let start = Date()
    private var last: CLLocation?
    private var lastTime: Date?
    private(set) var distance: Double = 0
    private(set) var movingTime: TimeInterval = 0
    private(set) var maxSpeed: Double = 0
    private var points: [[Double]] = []

    init(title: String) {
        self.title = title
    }

    func add(_ loc: CLLocation) {
        guard loc.horizontalAccuracy >= 0, loc.horizontalAccuracy < 35 else { return }
        if loc.speed > maxSpeed { maxSpeed = loc.speed }
        if let prev = last, let t = lastTime {
            let dt = loc.timestamp.timeIntervalSince(t)
            let d = loc.distance(from: prev)
            if dt > 0 && dt < 30 && (loc.speed > 1.5 || d / dt > 1.5) {
                movingTime += dt
            }
            lastTime = loc.timestamp
            guard d >= 10 else { return }
            distance += d
        } else {
            lastTime = loc.timestamp
        }
        last = loc
        points.append([loc.coordinate.latitude, loc.coordinate.longitude])
    }

    func finish() -> Trip? {
        guard distance > 200 else { return nil }
        var pts = points
        if pts.count > 3_000 {
            let step = Double(pts.count) / 3_000
            pts = stride(from: 0.0, to: Double(pts.count), by: step).map { pts[Int($0)] }
        }
        return Trip(id: UUID(), title: title, start: start, end: Date(), distance: distance,
                    movingTime: movingTime, maxSpeed: maxSpeed, points: pts)
    }
}

final class TripStore: ObservableObject {
    @Published private(set) var trips: [Trip] = []

    private var url: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("trips.json")
    }

    init() {
        if let data = try? Data(contentsOf: url), let list = try? JSONDecoder().decode([Trip].self, from: data) {
            trips = list
        }
    }

    func add(_ trip: Trip) {
        trips.insert(trip, at: 0)
        persist()
    }

    func delete(at offsets: IndexSet) {
        trips.remove(atOffsets: offsets)
        persist()
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(trips) {
            try? data.write(to: url, options: .atomic)
        }
    }
}
