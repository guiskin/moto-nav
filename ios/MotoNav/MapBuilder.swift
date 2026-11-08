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

    func unproject(x: Double, y: Double) -> CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: origin.latitude + y / ky, longitude: origin.longitude + x / kx)
    }
}

struct RoadLine {
    var kind: UInt8
    var points: [(Int32, Int32)]
}

struct RouteBlob {
    let data: Data
    let localCum: [Double]
}

enum MapBuilder {
    static func routeBlob(session: UInt16, projection proj: LocalProjection, geometry: RouteGeometry) -> RouteBlob {
        var routePts: [(Int32, Int32)] = []
        var cum: [Double] = []
        for c in geometry.coords {
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
        var d = Data("MNR2".utf8)
        append(&d, session)
        d.append(encodeLines([RoadLine(kind: 0, points: routePts)]))
        return RouteBlob(data: d, localCum: cum)
    }

    static func tileBlob(session: UInt16, tx: Int16, ty: Int16, lines: [RoadLine]) -> Data {
        var d = Data("MNT2".utf8)
        append(&d, session)
        append(&d, UInt16(bitPattern: tx))
        append(&d, UInt16(bitPattern: ty))
        d.append(encodeLines(lines))
        return d
    }

    static func encodeLines(_ lines: [RoadLine]) -> Data {
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
        var d = Data()
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

    static func append<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
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
        let maxspeed: Int?
        let name: String?
    }

    struct Camera {
        let coordinate: CLLocationCoordinate2D
        let maxspeed: Int?
    }

    struct Result {
        let ways: [Way]
        let cameras: [Camera]
    }

    static func parseSpeed(_ s: String?) -> Int? {
        guard let s = s?.trimmingCharacters(in: .whitespaces), !s.isEmpty else { return nil }
        let digits = s.prefix { $0.isNumber }
        guard let v = Int(digits), v > 0, v < 200 else { return nil }
        return s.lowercased().contains("mph") ? Int((Double(v) * 1.609).rounded()) : v
    }

    struct BBox {
        let south: Double
        let west: Double
        let north: Double
        let east: Double
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
            let lat: Double?
            let lon: Double?
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
        case "residential", "living_street", "road": return 4
        default: return nil
        }
    }

    static func fetchWays(in boxes: [BBox]) async throws -> Result {
        guard !boxes.isEmpty else { return Result(ways: [], cameras: []) }
        let filter = "^(motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|road|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link)$"
        let parts = boxes.map { b -> String in
            let box = String(format: "%.5f,%.5f,%.5f,%.5f", b.south, b.west, b.north, b.east)
            return "way[\"highway\"~\"\(filter)\"](\(box));node[\"highway\"=\"speed_camera\"](\(box));"
        }.joined()
        let query = "[out:json][timeout:25];(\(parts));out geom qt;"
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        let body = "data=" + (query.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")

        var lastError: Error = URLError(.badServerResponse)
        for endpoint in endpoints {
            guard let url = URL(string: endpoint) else { continue }
            var req = URLRequest(url: url, timeoutInterval: 30)
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
                var ways: [Way] = []
                var cameras: [Camera] = []
                for el in decoded.elements {
                    if el.type == "node", let la = el.lat, let lo = el.lon {
                        cameras.append(Camera(coordinate: CLLocationCoordinate2D(latitude: la, longitude: lo),
                                              maxspeed: parseSpeed(el.tags?["maxspeed"])))
                        continue
                    }
                    guard el.type == "way", let hw = el.tags?["highway"], let k = kind(for: hw),
                          let g = el.geometry, g.count >= 2 else { continue }
                    ways.append(Way(kind: k,
                                    points: g.map { CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lon) },
                                    maxspeed: parseSpeed(el.tags?["maxspeed"]),
                                    name: el.tags?["name"]))
                }
                return Result(ways: ways, cameras: cameras)
            } catch {
                lastError = error
            }
        }
        throw lastError
    }
}
