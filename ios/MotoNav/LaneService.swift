import Foundation
import CoreLocation

struct LaneInfo: Equatable {
    var dirs: [UInt8]
    var valid: UInt8

    static let left: UInt8 = 1
    static let through: UInt8 = 2
    static let right: UInt8 = 4
    static let reverse: UInt8 = 8

    func packet() -> Data {
        var d = Data([0x07, UInt8(dirs.count), valid])
        d.append(contentsOf: dirs)
        return d
    }

    static var empty: Data { Data([0x07, 0, 0]) }
}

enum LaneService {
    private struct Response: Decodable {
        struct Point: Decodable { let lat: Double; let lon: Double }
        struct Way: Decodable {
            let tags: [String: String]?
            let geometry: [Point]?
        }
        let elements: [Way]
    }

    static func need(for code: ManeuverCode) -> UInt8? {
        switch code {
        case .right, .sharpRight, .slightRight: return LaneInfo.right
        case .left, .sharpLeft, .slightLeft: return LaneInfo.left
        case .straight: return LaneInfo.through
        case .uturn: return LaneInfo.left | LaneInfo.reverse
        default: return nil
        }
    }

    static func parse(_ value: String) -> [UInt8] {
        value.split(separator: "|", omittingEmptySubsequences: false).map { lane in
            var bits: UInt8 = 0
            for t in lane.split(separator: ";") {
                switch t.trimmingCharacters(in: .whitespaces) {
                case "left", "slight_left", "sharp_left": bits |= LaneInfo.left
                case "right", "slight_right", "sharp_right": bits |= LaneInfo.right
                case "reverse": bits |= LaneInfo.reverse
                default: bits |= LaneInfo.through
                }
            }
            return bits == 0 ? LaneInfo.through : bits
        }
    }

    static func fetch(near p: CLLocationCoordinate2D, bearing: Double, code: ManeuverCode) async -> LaneInfo? {
        guard let need = need(for: code) else { return nil }
        let q = "[out:json][timeout:8];way(around:25,\(p.latitude),\(p.longitude))[\"highway\"];out tags geom;"
        var comps = URLComponents(string: "https://overpass-api.de/api/interpreter")!
        comps.queryItems = [URLQueryItem(name: "data", value: q)]
        guard let url = comps.url else { return nil }
        var req = URLRequest(url: url, timeoutInterval: 9)
        req.setValue("MotoNav/1.0", forHTTPHeaderField: "User-Agent")
        guard let (data, _) = try? await URLSession.shared.data(for: req),
              let resp = try? JSONDecoder().decode(Response.self, from: data) else { return nil }

        var best: (score: Double, way: Response.Way, forward: Bool)?
        for w in resp.elements {
            guard let g = w.geometry, g.count > 1, let tags = w.tags else { continue }
            var nearest = Double.infinity
            var segBearing = 0.0
            for i in 0..<(g.count - 1) {
                let a = CLLocationCoordinate2D(latitude: g[i].lat, longitude: g[i].lon)
                let b = CLLocationCoordinate2D(latitude: g[i + 1].lat, longitude: g[i + 1].lon)
                let d = distanceToSegment(p, a, b)
                if d < nearest { nearest = d; segBearing = Geo.bearing(a, b) }
            }
            let diff = abs(Geo.normalize(segBearing - bearing))
            let forward: Bool
            if diff < 35 { forward = true } else if diff > 145 { forward = false } else { continue }
            guard tags["turn:lanes"] != nil || tags["turn:lanes:forward"] != nil || tags["turn:lanes:backward"] != nil else { continue }
            if best == nil || nearest < best!.score { best = (nearest, w, forward) }
        }
        guard let b = best, let tags = b.way.tags else { return nil }
        let oneway = tags["oneway"] ?? "no"
        let value: String?
        if oneway == "yes" || oneway == "true" || oneway == "1" {
            guard b.forward else { return nil }
            value = tags["turn:lanes"] ?? tags["turn:lanes:forward"]
        } else if oneway == "-1" {
            guard !b.forward else { return nil }
            value = tags["turn:lanes"] ?? tags["turn:lanes:backward"]
        } else {
            value = b.forward ? tags["turn:lanes:forward"] : tags["turn:lanes:backward"]
        }
        guard let v = value else { return nil }
        let dirs = parse(v)
        guard dirs.count >= 2, dirs.count <= 8 else { return nil }
        var mask: UInt8 = 0
        for (i, d) in dirs.enumerated() where d & need != 0 { mask |= UInt8(1 << i) }
        let all = UInt8((1 << dirs.count) - 1)
        guard mask != 0, mask != all else { return nil }
        return LaneInfo(dirs: dirs, valid: mask)
    }

    private static func distanceToSegment(_ p: CLLocationCoordinate2D, _ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let k = 111_320.0
        let cosLat = cos(p.latitude * .pi / 180)
        let ax = (a.longitude - p.longitude) * k * cosLat, ay = (a.latitude - p.latitude) * k
        let bx = (b.longitude - p.longitude) * k * cosLat, by = (b.latitude - p.latitude) * k
        let dx = bx - ax, dy = by - ay
        let l2 = dx * dx + dy * dy
        let t = l2 == 0 ? 0 : max(0, min(1, -(ax * dx + ay * dy) / l2))
        let cx = ax + t * dx, cy = ay + t * dy
        return (cx * cx + cy * cy).squareRoot()
    }
}
