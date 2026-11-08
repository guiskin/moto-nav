import Foundation
import MapKit

enum ManeuverCode: UInt8 {
    case none = 0
    case straight = 1
    case slightRight = 2
    case right = 3
    case sharpRight = 4
    case slightLeft = 5
    case left = 6
    case sharpLeft = 7
    case uturn = 8
    case roundabout = 9
    case arrive = 10
    case depart = 11
    case keepRight = 12
    case keepLeft = 13

    var symbol: String {
        switch self {
        case .none, .straight, .depart: return "arrow.up"
        case .slightRight, .keepRight: return "arrow.up.right"
        case .right, .sharpRight: return "arrow.turn.up.right"
        case .slightLeft, .keepLeft: return "arrow.up.left"
        case .left, .sharpLeft: return "arrow.turn.up.left"
        case .uturn: return "arrow.uturn.down"
        case .roundabout: return "arrow.triangle.turn.up.right.circle"
        case .arrive: return "flag.checkered"
        }
    }
}

struct Maneuver {
    var code: ManeuverCode
    var exitNumber: UInt8
    var angle: Int16
}

enum Geo {
    static func distance(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let r = 6_371_000.0
        let lat1 = a.latitude * .pi / 180, lat2 = b.latitude * .pi / 180
        let dLat = lat2 - lat1
        let dLon = (b.longitude - a.longitude) * .pi / 180
        let h = sin(dLat / 2) * sin(dLat / 2) + cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * r * atan2(sqrt(h), sqrt(1 - h))
    }

    static func bearing(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let lat1 = a.latitude * .pi / 180, lat2 = b.latitude * .pi / 180
        let dLon = (b.longitude - a.longitude) * .pi / 180
        let y = sin(dLon) * cos(lat2)
        let x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon)
        let deg = atan2(y, x) * 180 / .pi
        return (deg + 360).truncatingRemainder(dividingBy: 360)
    }

    static func normalize(_ deg: Double) -> Double {
        var d = deg.truncatingRemainder(dividingBy: 360)
        if d > 180 { d -= 360 }
        if d <= -180 { d += 360 }
        return d
    }
}

extension MKPolyline {
    var coordinateArray: [CLLocationCoordinate2D] {
        var coords = [CLLocationCoordinate2D](repeating: kCLLocationCoordinate2DInvalid, count: pointCount)
        getCoordinates(&coords, range: NSRange(location: 0, length: pointCount))
        return coords
    }
}

struct RouteGeometry {
    private(set) var coords: [CLLocationCoordinate2D] = []
    private(set) var cum: [Double] = []
    private(set) var stepStart: [Double] = []
    private(set) var instructions: [String] = []
    private(set) var curves: [Curve] = []

    struct Curve {
        let along: Double
        let angle: Double
    }

    var total: Double { cum.last ?? 0 }

    init(route: MKRoute) {
        for step in route.steps {
            stepStart.append(cum.last ?? 0)
            instructions.append(step.instructions)
            for p in step.polyline.coordinateArray {
                append(p)
            }
        }
        if coords.count < 2 {
            coords = []
            cum = []
            for p in route.polyline.coordinateArray { append(p) }
        }
        curves = findCurves()
    }

    private func findCurves() -> [Curve] {
        guard total > 120 else { return [] }
        let step = 10.0
        let w = 8
        let n = Int(total / step)
        var bearings: [Double] = []
        bearings.reserveCapacity(n)
        for i in 0..<n {
            let d = Double(i) * step
            bearings.append(Geo.bearing(point(at: d), point(at: d + step)))
        }
        guard bearings.count > w + 1 else { return [] }
        let maneuvers = Array(stepStart.dropFirst())
        var result: [Curve] = []
        var i = 0
        while i < bearings.count - w {
            let d = Geo.normalize(bearings[i + w] - bearings[i])
            guard abs(d) >= 45 else {
                i += 1
                continue
            }
            var best = d
            var j = i
            while j < bearings.count - w {
                let dj = Geo.normalize(bearings[j + w] - bearings[j])
                if abs(dj) < 45 || (dj > 0) != (best > 0) { break }
                if abs(dj) > abs(best) { best = dj }
                j += 1
            }
            let center = Double(i) * step + Double(w) * step / 2
            if !maneuvers.contains(where: { abs($0 - center) < 60 }) {
                result.append(Curve(along: Double(i) * step + 20, angle: best))
            }
            i = j + w / 2
        }
        return result
    }

    func nextCurve(after along: Double, within range: Double) -> (index: Int, curve: Curve)? {
        for (i, c) in curves.enumerated() where c.along > along - 10 {
            return c.along - along <= range ? (i, c) : nil
        }
        return nil
    }

    private mutating func append(_ p: CLLocationCoordinate2D) {
        guard CLLocationCoordinate2DIsValid(p) else { return }
        if let last = coords.last {
            let d = Geo.distance(last, p)
            if d < 0.05 { return }
            coords.append(p)
            cum.append((cum.last ?? 0) + d)
        } else {
            coords.append(p)
            cum.append(0)
        }
    }

    func point(at distance: Double) -> CLLocationCoordinate2D {
        guard coords.count > 1 else { return coords.first ?? kCLLocationCoordinate2DInvalid }
        let d = min(max(distance, 0), total)
        var lo = 0, hi = cum.count - 1
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if cum[mid] <= d { lo = mid } else { hi = mid }
        }
        let seg = cum[hi] - cum[lo]
        let t = seg > 0 ? (d - cum[lo]) / seg : 0
        let a = coords[lo], b = coords[hi]
        return CLLocationCoordinate2D(latitude: a.latitude + (b.latitude - a.latitude) * t,
                                      longitude: a.longitude + (b.longitude - a.longitude) * t)
    }

    func bearing(at distance: Double) -> Double {
        let a = point(at: distance - 5)
        let b = point(at: distance + 5)
        return Geo.bearing(a, b)
    }

    struct Projection {
        var along: Double
        var offRoute: Double
        var segment: Int
    }

    func project(_ p: CLLocationCoordinate2D, near hint: Int?) -> Projection? {
        guard coords.count > 1 else { return nil }
        let n = coords.count - 1
        var best: Projection?
        func scan(_ range: ClosedRange<Int>) {
            let k = cos(p.latitude * .pi / 180) * 111_320.0
            for i in range {
                let a = coords[i], b = coords[i + 1]
                let ax = (a.longitude - p.longitude) * k, ay = (a.latitude - p.latitude) * 110_540.0
                let bx = (b.longitude - p.longitude) * k, by = (b.latitude - p.latitude) * 110_540.0
                let dx = bx - ax, dy = by - ay
                let len2 = dx * dx + dy * dy
                var t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0
                t = min(max(t, 0), 1)
                let cx = ax + dx * t, cy = ay + dy * t
                let dist = sqrt(cx * cx + cy * cy)
                if best == nil || dist < best!.offRoute {
                    best = Projection(along: cum[i] + (cum[i + 1] - cum[i]) * t, offRoute: dist, segment: i)
                }
            }
        }
        if let h = hint {
            let lo = max(0, h - 10), hi = min(n - 1, h + 400)
            if lo <= hi { scan(lo...hi) }
            if let b = best, b.offRoute < 40 { return b }
        }
        best = nil
        scan(0...(n - 1))
        return best
    }

    func nextStepIndex(after along: Double) -> Int? {
        guard stepStart.count > 1 else { return nil }
        for i in 1..<stepStart.count where stepStart[i] > along + 3 {
            if instructions[i].trimmingCharacters(in: .whitespaces).isEmpty && i != stepStart.count - 1 { continue }
            return i
        }
        return nil
    }

    func maneuver(forStep k: Int) -> Maneuver {
        let text = instructions[k].lowercased()
        if k == instructions.count - 1 || text.contains("destino") || text.contains("destination") {
            return Maneuver(code: .arrive, exitNumber: 0, angle: 0)
        }
        let at = stepStart[k]
        if at < 1 { return Maneuver(code: .depart, exitNumber: 0, angle: 0) }

        let isRoundabout = ["rotatória", "rotatoria", "rótula", "rotula", "roundabout", "balão"].contains { text.contains($0) }
        let inB = Geo.bearing(point(at: at - 30), point(at: at))
        let outB = isRoundabout
            ? Geo.bearing(point(at: at + 70), point(at: at + 100))
            : Geo.bearing(point(at: at), point(at: at + 25))
        var angle = Geo.normalize(outB - inB)

        if isRoundabout {
            return Maneuver(code: .roundabout, exitNumber: Self.exitNumber(in: text), angle: Int16(angle.rounded()))
        }

        let saysRight = text.contains("direita") || text.contains("right")
        let saysLeft = text.contains("esquerda") || text.contains("left")
        if saysRight && !saysLeft && angle < 0 { angle = -angle }
        if saysLeft && !saysRight && angle > 0 { angle = -angle }

        if ["retorno", "meia-volta", "meia volta", "u-turn", "make a u"].contains(where: { text.contains($0) }) {
            return Maneuver(code: .uturn, exitNumber: 0, angle: 180)
        }
        if text.contains("mantenha") || text.contains("permaneça") || text.contains("keep") {
            if saysRight { return Maneuver(code: .keepRight, exitNumber: 0, angle: 30) }
            if saysLeft { return Maneuver(code: .keepLeft, exitNumber: 0, angle: -30) }
        }
        if text.contains("saída") || text.contains("exit") || text.contains("acesso") {
            if saysRight && abs(angle) < 60 { return Maneuver(code: .slightRight, exitNumber: 0, angle: 45) }
            if saysLeft && abs(angle) < 60 { return Maneuver(code: .slightLeft, exitNumber: 0, angle: -45) }
        }

        let turnWord = text.contains("vire") || text.contains("turn") || text.contains("dobre")
        let slightWord = text.contains("suave") || text.contains("ligeiramente") || text.contains("slight")
        let sharpWord = text.contains("acentuada") || text.contains("fechada") || text.contains("sharp")
        let mag = abs(angle)
        let right = angle >= 0

        let code: ManeuverCode
        if sharpWord || mag >= 135 {
            code = mag >= 170 ? .uturn : (right ? .sharpRight : .sharpLeft)
        } else if slightWord {
            code = right ? .slightRight : .slightLeft
        } else if turnWord && (saysRight || saysLeft) {
            code = right ? .right : .left
        } else if mag < 20 {
            code = .straight
        } else if mag < 60 {
            code = right ? .slightRight : .slightLeft
        } else {
            code = right ? .right : .left
        }
        return Maneuver(code: code, exitNumber: 0, angle: Int16(angle.rounded()))
    }

    static func exitNumber(in text: String) -> UInt8 {
        let ordinals: [(String, UInt8)] = [
            ("primeira", 1), ("segunda", 2), ("terceira", 3), ("quarta", 4), ("quinta", 5), ("sexta", 6),
            ("first", 1), ("second", 2), ("third", 3), ("fourth", 4), ("fifth", 5), ("sixth", 6)
        ]
        if let r = text.range(of: #"(\d+)\s*(ª|a|º|o|°|st|nd|rd|th)?\s*(saída|saida|exit)"#, options: .regularExpression) {
            let digits = text[r].prefix { $0.isNumber }
            if let n = UInt8(digits) { return n }
        }
        for (word, n) in ordinals where text.contains(word) { return n }
        return 0
    }

    static func street(from instruction: String) -> String {
        var s = instruction.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasSuffix(".") { s.removeLast() }
        let lower = s.lowercased()
        if lower.contains("destino") || lower.contains("destination") { return s }
        let towards = [" em direção a ", " em direção à ", " em direção ao ", " sentido ", " towards ", " toward "]
        for m in towards {
            if let r = s.range(of: m, options: .caseInsensitive) {
                let tail = s[r.upperBound...].trimmingCharacters(in: .whitespaces)
                if tail.count >= 3 { return "Sentido " + tail }
            }
        }
        let markers = [" para a ", " para o ", " para ", " na ", " no ", " em ", " pela ", " pelo ", " onto ", " on "]
        var first: Range<String.Index>?
        for m in markers {
            if let r = s.range(of: m, options: .caseInsensitive) {
                if first == nil || r.lowerBound < first!.lowerBound { first = r }
            }
        }
        if let r = first {
            let tail = String(s[r.upperBound...]).trimmingCharacters(in: .whitespaces)
            if tail.count >= 3 { return tail }
        }
        return s
    }
}
