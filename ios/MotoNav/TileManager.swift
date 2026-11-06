import Foundation
import CoreLocation

struct TileKey: Hashable {
    let x: Int16
    let y: Int16
}

@MainActor
final class TileManager {
    nonisolated static let size = 800.0

    let session: UInt16
    let projection: LocalProjection
    var upload: ((Data) -> Void)?
    var onStatus: ((String?) -> Void)?

    private var blobs: [TileKey: Data] = [:]
    private var sent: Set<TileKey> = []
    private var inFlight: Set<TileKey> = []
    private var failedAt: [TileKey: Date] = [:]
    private var fetching = false
    private var lastDesired: [TileKey] = []

    init(session: UInt16, projection: LocalProjection) {
        self.session = session
        self.projection = projection
    }

    func key(x: Double, y: Double) -> TileKey {
        TileKey(x: Int16(clamping: Int(floor(x / Self.size))), y: Int16(clamping: Int(floor(y / Self.size))))
    }

    func resetSent() {
        sent.removeAll()
    }

    func update(position: (x: Double, y: Double), ahead: [(x: Double, y: Double)]) {
        let center = key(x: position.x, y: position.y)
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
        for p in ahead { add(key(x: p.x, y: p.y)) }
        desired.sort { distance($0, position) < distance($1, position) }
        lastDesired = desired
        pump(position: position)
    }

    private func distance(_ k: TileKey, _ p: (x: Double, y: Double)) -> Double {
        let cx = (Double(k.x) + 0.5) * Self.size, cy = (Double(k.y) + 0.5) * Self.size
        return hypot(cx - p.x, cy - p.y)
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
            let ways = try await OverpassClient.fetchWays(in: boxes)
            let built = await Task.detached(priority: .userInitiated) {
                Self.split(ways: ways, keys: batch, projection: proj, session: session)
            }.value
            for k in batch {
                blobs[k] = built[k] ?? MapBuilder.tileBlob(session: session, tx: k.x, ty: k.y, lines: [])
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

    nonisolated private static func split(ways: [OverpassClient.Way], keys: [TileKey], projection: LocalProjection, session: UInt16) -> [TileKey: Data] {
        let wanted = Set(keys)
        var runs: [TileKey: [(kind: UInt8, pts: [(Double, Double)])]] = [:]
        func tileOf(_ p: (Double, Double)) -> TileKey {
            TileKey(x: Int16(clamping: Int(floor(p.0 / size))), y: Int16(clamping: Int(floor(p.1 / size))))
        }
        for way in ways {
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
            var open: [TileKey: Int] = [:]
            for i in 0..<(pts.count - 1) {
                let a = pts[i], b = pts[i + 1]
                var tiles = [tileOf(a)]
                let tb = tileOf(b)
                if tb != tiles[0] { tiles.append(tb) }
                for t in tiles where wanted.contains(t) {
                    if let idx = open[t], let lastPt = runs[t]?[idx].pts.last, lastPt == a {
                        runs[t]![idx].pts.append(b)
                    } else {
                        runs[t, default: []].append((kind: way.kind, pts: [a, b]))
                        open[t] = runs[t]!.count - 1
                    }
                }
            }
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
        return out
    }
}
