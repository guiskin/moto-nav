import Foundation
import CoreLocation

struct LocalProjection {
    let origin: CLLocationCoordinate2D
    let kx: Double
    let ky: Double = 110_540.0

    init(origin: CLLocationCoordinate2D) {
        self.origin = origin
        kx = cos(origin.latitude * .pi / 180) * 111_320.0
    }

    func project(_ c: CLLocationCoordinate2D) -> (x: Double, y: Double) {
        ((c.longitude - origin.longitude) * kx, (c.latitude - origin.latitude) * ky)
    }
}

struct RoadLine {
    var kind: UInt8
    var points: [(Int32, Int32)]
}

struct BuiltMap {
    let id: UInt16
    let projection: LocalProjection
    let routeLocalCum: [Double]
    let blob: Data
    let roadCount: Int
}

enum MapBuilder {
    static func build(id: UInt16, geometry: RouteGeometry) async -> BuiltMap {
        let coords = geometry.coords
        let origin = coords.first ?? CLLocationCoordinate2D(latitude: 0, longitude: 0)
        let proj = LocalProjection(origin: origin)

        var routePts: [(Int32, Int32)] = []
        var cum: [Double] = []
        for c in coords {
            let p = proj.project(c)
            let q = (Int32(p.x.rounded()), Int32(p.y.rounded()))
            if let last = routePts.last {
                let dx = Double(q.0 - last.0), dy = Double(q.1 - last.1)
                cum.append(cum.last! + sqrt(dx * dx + dy * dy))
            } else {
                cum.append(0)
            }
            routePts.append(q)
        }

        var roads: [RoadLine] = []
        if let fetched = try? await OverpassClient.fetchRoads(along: coords, total: geometry.total) {
            roads = fetched.compactMap { way in
                let pts = way.points.map { c -> (Double, Double) in
                    let p = proj.project(c)
                    return (p.x, p.y)
                }
                let simple = simplify(pts, tolerance: 2.0)
                guard simple.count >= 2 else { return nil }
                return RoadLine(kind: way.kind, points: simple.map { (Int32($0.0.rounded()), Int32($0.1.rounded())) })
            }
            roads = cap(roads, maxPoints: 120_000)
        }

        let lines = [RoadLine(kind: 0, points: routePts)] + roads
        let blob = encode(lines)
        return BuiltMap(id: id, projection: proj, routeLocalCum: cum, blob: blob, roadCount: roads.count)
    }

    private static func cap(_ roads: [RoadLine], maxPoints: Int) -> [RoadLine] {
        var r = roads
        var total = r.reduce(0) { $0 + $1.points.count }
        for dropKind: UInt8 in [4, 3] where total > maxPoints {
            r.removeAll { $0.kind == dropKind }
            total = r.reduce(0) { $0 + $1.points.count }
        }
        return r
    }

    static func encode(_ lines: [RoadLine]) -> Data {
        var d = Data("MNM1".utf8)
        var out: [RoadLine] = []
        for line in lines {
            var pts: [(Int32, Int32)] = []
            for p in line.points {
                if let last = pts.last {
                    let dx = Int(p.0) - Int(last.0), dy = Int(p.1) - Int(last.1)
                    let steps = max(1, Int((Double(max(abs(dx), abs(dy))) / 30_000).rounded(.up)))
                    if steps > 1 {
                        for s in 1..<steps {
                            let f = Double(s) / Double(steps)
                            pts.append((last.0 + Int32((Double(dx) * f).rounded()), last.1 + Int32((Double(dy) * f).rounded())))
                        }
                    }
                }
                pts.append(p)
            }
            var start = 0
            while start < pts.count - 1 {
                let end = min(pts.count, start + 60_000)
                out.append(RoadLine(kind: line.kind, points: Array(pts[start..<end])))
                start = end - 1
            }
        }
        if out.count > 65_000 { out = Array(out.prefix(65_000)) }
        append(&d, UInt16(out.count))
        for line in out {
            d.append(line.kind)
            append(&d, UInt16(line.points.count))
            append(&d, UInt32(bitPattern: line.points[0].0))
            append(&d, UInt32(bitPattern: line.points[0].1))
            var prev = line.points[0]
            for p in line.points.dropFirst() {
                append(&d, UInt16(bitPattern: Int16(p.0 - prev.0)))
                append(&d, UInt16(bitPattern: Int16(p.1 - prev.1)))
                prev = p
            }
        }
        return d
    }

    private static func append<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
        withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) }
    }

    static func simplify(_ pts: [(Double, Double)], tolerance: Double) -> [(Double, Double)] {
        guard pts.count > 2 else { return pts }
        var keep = [Bool](repeating: false, count: pts.count)
        keep[0] = true
        keep[pts.count - 1] = true
        var stack = [(0, pts.count - 1)]
        while let seg = stack.popLast() {
            let (a, b) = seg
            guard b > a + 1 else { continue }
            let (ax, ay) = pts[a], (bx, by) = pts[b]
            let dx = bx - ax, dy = by - ay
            let len2 = dx * dx + dy * dy
            var maxD = -1.0
            var idx = a
            for i in (a + 1)..<b {
                let (px, py) = pts[i]
                var t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0
                t = min(max(t, 0), 1)
                let cx = ax + dx * t - px, cy = ay + dy * t - py
                let dist = sqrt(cx * cx + cy * cy)
                if dist > maxD {
                    maxD = dist
                    idx = i
                }
            }
            if maxD > tolerance {
                keep[idx] = true
                stack.append((a, idx))
                stack.append((idx, b))
            }
        }
        return pts.enumerated().compactMap { keep[$0.offset] ? $0.element : nil }
    }
}

enum OverpassClient {
    struct Way {
        let kind: UInt8
        let points: [CLLocationCoordinate2D]
    }

    private struct Response: Decodable {
        struct Element: Decodable {
            struct LatLon: Decodable {
                let lat: Double
                let lon: Double
            }
            let type: String
            let tags: [String: String]?
            let geometry: [LatLon]?
        }
        let elements: [Element]
    }

    private static let endpoints = [
        "https://overpass-api.de/api/interpreter",
        "https://overpass.kumi.systems/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter"
    ]

    static func kind(for highway: String) -> UInt8? {
        switch highway {
        case "motorway", "trunk", "motorway_link", "trunk_link": return 1
        case "primary", "secondary", "primary_link", "secondary_link": return 2
        case "tertiary", "tertiary_link", "unclassified": return 3
        case "residential", "living_street": return 4
        default: return nil
        }
    }

    static func fetchRoads(along coords: [CLLocationCoordinate2D], total: Double) async throws -> [Way] {
        guard coords.count >= 2 else { return [] }
        let radius: Int = total < 30_000 ? 300 : (total < 100_000 ? 200 : 120)
        let spacing = max(120.0, total / 300.0)
        var samples: [CLLocationCoordinate2D] = [coords[0]]
        var acc = 0.0
        for i in 1..<coords.count {
            acc += Geo.distance(coords[i - 1], coords[i])
            if acc >= spacing {
                samples.append(coords[i])
                acc = 0
            }
        }
        if let last = coords.last { samples.append(last) }
        let line = samples.map { String(format: "%.5f,%.5f", $0.latitude, $0.longitude) }.joined(separator: ",")
        let filter = total < 100_000
            ? "^(motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link)$"
            : "^(motorway|trunk|primary|secondary|tertiary|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link)$"
        let query = "[out:json][timeout:60];way[\"highway\"~\"\(filter)\"](around:\(radius),\(line));out geom qt;"
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        let body = "data=" + (query.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")

        var lastError: Error = URLError(.badServerResponse)
        for endpoint in endpoints {
            guard let url = URL(string: endpoint) else { continue }
            var req = URLRequest(url: url, timeoutInterval: 75)
            req.httpMethod = "POST"
            req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
            req.setValue("MotoNav/1.0 (iOS; uso pessoal)", forHTTPHeaderField: "User-Agent")
            req.httpBody = body.data(using: .utf8)
            do {
                let (data, resp) = try await URLSession.shared.data(for: req)
                guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
                    lastError = URLError(.badServerResponse)
                    continue
                }
                let decoded = try JSONDecoder().decode(Response.self, from: data)
                return decoded.elements.compactMap { el in
                    guard el.type == "way", let hw = el.tags?["highway"], let k = kind(for: hw),
                          let g = el.geometry, g.count >= 2 else { return nil }
                    return Way(kind: k, points: g.map { CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lon) })
                }
            } catch {
                lastError = error
            }
        }
        throw lastError
    }
}
