import Foundation
import CoreLocation

struct TileKey: Hashable {
    let x: Int16
    let y: Int16
}

struct RoadInfo {
    let maxspeed: Int?
    let name: String?
    let pts: [(Double, Double)]
}

struct CameraInfo {
    let x: Double
    let y: Double
    let maxspeed: Int?
}

struct TileFeatures {
    var roads: [RoadInfo] = []
    var cameras: [CameraInfo] = []
}

@MainActor
final class TileManager {
    nonisolated static let size = 800.0

    let session: UInt16
    let projection: LocalProjection
    var upload: ((Data) -> Void)?
    var onStatus: ((String?) -> Void)?

    private var blobs: [TileKey: Data] = [:]
    private var features: [TileKey: TileFeatures] = [:]
    private var sent: Set<TileKey> = []
    private var inFlight: Set<TileKey> = []
    private var failedAt: [TileKey: Date] = [:]
    private var fetching = false
    private var lastDesired: [TileKey] = []

    init(session: UInt16, projection: LocalProjection) {
        self.session = session
        self.projection = projection
    }

    nonisolated static func key(x: Double, y: Double) -> TileKey {
        TileKey(x: Int16(clamping: Int(floor(x / size))), y: Int16(clamping: Int(floor(y / size))))
    }

    func resetSent() {
        sent.removeAll()
    }

    func update(position: (x: Double, y: Double), ahead: [(x: Double, y: Double)]) {
        let center = Self.key(x: position.x, y: position.y)
        var desired: [TileKey] = []
        var seen = Set<TileKey>()
        func add(_ k: TileKey) {
            if seen.insert(k).inserted { desired.append(k) }
        }
        add(center)
        for dy in -1...1 {
            for dx in -1...1 {
                add(TileKey(x: center.x &+ Int16(dx), y: center.y &+ Int16(dy)))
            }
        }
        for p in ahead { add(Self.key(x: p.x, y: p.y)) }
        desired.sort { distance($0, position) < distance($1, position) }
        lastDesired = desired
        pump(position: position)
    }

    private func distance(_ k: TileKey, _ p: (x: Double, y: Double)) -> Double {
        let cx = (Double(k.x) + 0.5) * Self.size, cy = (Double(k.y) + 0.5) * Self.size
        return hypot(cx - p.x, cy - p.y)
    }

    private func nearbyFeatures(_ p: (x: Double, y: Double)) -> [TileFeatures] {
        let c = Self.key(x: p.x, y: p.y)
        var out: [TileFeatures] = []
        for dy in -1...1 {
            for dx in -1...1 {
                if let f = features[TileKey(x: c.x &+ Int16(dx), y: c.y &+ Int16(dy))] { out.append(f) }
            }
        }
        return out
    }

    private static func angleDiff(_ a: Double, _ b: Double) -> Double {
        var d = (a - b).truncatingRemainder(dividingBy: 360)
        if d > 180 { d -= 360 }
        if d < -180 { d += 360 }
        return abs(d)
    }

    func roadInfo(at p: (x: Double, y: Double), course: Double?) -> (limit: Int?, name: String?) {
        var best: (score: Double, road: RoadInfo)?
        for f in nearbyFeatures(p) {
            for road in f.roads {
                for i in 0..<(road.pts.count - 1) {
                    let a = road.pts[i], b = road.pts[i + 1]
                    let dx = b.0 - a.0, dy = b.1 - a.1
                    let len2 = dx * dx + dy * dy
                    var t = len2 > 0 ? ((p.x - a.0) * dx + (p.y - a.1) * dy) / len2 : 0
                    t = min(max(t, 0), 1)
                    let d = hypot(a.0 + dx * t - p.x, a.1 + dy * t - p.y)
                    guard d < 20 else { continue }
                    var score = d
                    if let c = course, len2 > 1 {
                        let segB = atan2(dx, dy) * 180 / .pi
                        let diff = min(Self.angleDiff(segB, c), Self.angleDiff(segB + 180, c))
                        score += diff * 0.4
                    }
                    if best == nil || score < best!.score { best = (score, road) }
                }
            }
        }
        return (best?.road.maxspeed, best?.road.name)
    }

    func nextCamera(at p: (x: Double, y: Double), course: Double?) -> (key: String, distance: Double, limit: Int?)? {
        var best: (key: String, distance: Double, limit: Int?)?
        for f in nearbyFeatures(p) {
            for cam in f.cameras {
                let d = hypot(cam.x - p.x, cam.y - p.y)
                guard d <= 700 else { continue }
                if let c = course {
                    let b = atan2(cam.x - p.x, cam.y - p.y) * 180 / .pi
                    guard d < 25 || Self.angleDiff(b, c) < 30 else { continue }
                }
                if best == nil || d < best!.distance {
                    best = (String(format: "%.0f,%.0f", cam.x, cam.y), d, cam.maxspeed)
                }
            }
        }
        return best
    }

    private func pump(position: (x: Double, y: Double)) {
        for k in lastDesired where !sent.contains(k) {
            if let b = blobs[k] {
                upload?(b)
                sent.insert(k)
            }
        }
        guard !fetching else { return }
        let now = Date()
        let missing = lastDesired.filter { k in
            blobs[k] == nil && !inFlight.contains(k) && (failedAt[k].map { now.timeIntervalSince($0) > 20 } ?? true)
        }
        guard !missing.isEmpty else { return }
        let batch = Array(missing.prefix(9))
        fetching = true
        batch.forEach { inFlight.insert($0) }
        onStatus?("Baixando ruas...")
        Task { [weak self] in
            await self?.fetch(batch, position: position)
        }
    }

    private func fetch(_ batch: [TileKey], position: (x: Double, y: Double)) async {
        let boxes = batch.map { k -> OverpassClient.BBox in
            let x0 = Double(k.x) * Self.size, y0 = Double(k.y) * Self.size
            let sw = projection.unproject(x: x0 - 20, y: y0 - 20)
            let ne = projection.unproject(x: x0 + Self.size + 20, y: y0 + Self.size + 20)
            return OverpassClient.BBox(south: sw.latitude, west: sw.longitude, north: ne.latitude, east: ne.longitude)
        }
        let proj = projection
        let session = self.session
        do {
            let result = try await OverpassClient.fetchWays(in: boxes)
            let built = await Task.detached(priority: .userInitiated) {
                Self.split(result: result, keys: batch, projection: proj, session: session)
            }.value
            for k in batch {
                blobs[k] = built.blobs[k] ?? MapBuilder.tileBlob(session: session, tx: k.x, ty: k.y, lines: [])
                features[k] = built.features[k] ?? TileFeatures()
                failedAt[k] = nil
            }
            onStatus?(nil)
        } catch {
            let now = Date()
            for k in batch { failedAt[k] = now }
            onStatus?("Sem dados de ruas (verifique a internet)")
        }
        batch.forEach { inFlight.remove($0) }
        fetching = false
        pump(position: position)
    }

    nonisolated private static func split(result: OverpassClient.Result, keys: [TileKey], projection: LocalProjection,
                                          session: UInt16) -> (blobs: [TileKey: Data], features: [TileKey: TileFeatures]) {
        let wanted = Set(keys)
        var runs: [TileKey: [(kind: UInt8, pts: [(Double, Double)])]] = [:]
        var feats: [TileKey: TileFeatures] = [:]
        for way in result.ways {
            var pts: [(Double, Double)] = []
            for c in way.points {
                let p = projection.project(c)
                let q = (p.x, p.y)
                if let last = pts.last {
                    let d = hypot(q.0 - last.0, q.1 - last.1)
                    let steps = Int(d / 200)
                    if steps > 0 {
                        for s in 1...steps {
                            let f = Double(s) / Double(steps + 1)
                            pts.append((last.0 + (q.0 - last.0) * f, last.1 + (q.1 - last.1) * f))
                        }
                    }
                }
                pts.append(q)
            }
            var touched = Set<TileKey>()
            var open: [TileKey: Int] = [:]
            for i in 0..<(pts.count - 1) {
                let a = pts[i], b = pts[i + 1]
                var tiles = [key(x: a.0, y: a.1)]
                let tb = key(x: b.0, y: b.1)
                if tb != tiles[0] { tiles.append(tb) }
                for t in tiles where wanted.contains(t) {
                    touched.insert(t)
                    if let idx = open[t], let lastPt = runs[t]?[idx].pts.last, lastPt == a {
                        runs[t]![idx].pts.append(b)
                    } else {
                        runs[t, default: []].append((kind: way.kind, pts: [a, b]))
                        open[t] = runs[t]!.count - 1
                    }
                }
            }
            if way.maxspeed != nil || way.name != nil {
                let info = RoadInfo(maxspeed: way.maxspeed, name: way.name, pts: MapBuilder.simplify(pts, tolerance: 2.0))
                for t in touched { feats[t, default: TileFeatures()].roads.append(info) }
            }
        }
        for cam in result.cameras {
            let p = projection.project(cam.coordinate)
            let k = key(x: p.x, y: p.y)
            guard wanted.contains(k) else { continue }
            feats[k, default: TileFeatures()].cameras.append(CameraInfo(x: p.x, y: p.y, maxspeed: cam.maxspeed))
        }
        var out: [TileKey: Data] = [:]
        for (t, list) in runs {
            let lines = list.compactMap { run -> RoadLine? in
                let simple = MapBuilder.simplify(run.pts, tolerance: 2.0)
                guard simple.count >= 2 else { return nil }
                return RoadLine(kind: run.kind, points: simple.map { (Int32($0.0.rounded()), Int32($0.1.rounded())) })
            }
            out[t] = MapBuilder.tileBlob(session: session, tx: t.x, ty: t.y, lines: lines)
        }
        return (out, feats)
    }
}
