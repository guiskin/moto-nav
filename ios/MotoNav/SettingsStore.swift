import Foundation
import CoreLocation

final class SettingsStore: ObservableObject {
    enum NightMode: Int, CaseIterable, Identifiable {
        case auto = 0
        case always = 1
        case never = 2
        var id: Int { rawValue }
        var label: String {
            switch self {
            case .auto: return "Automático"
            case .always: return "Sempre"
            case .never: return "Nunca"
            }
        }
    }

    @Published var dayBrightness: Double { didSet { save() } }
    @Published var nightBrightness: Double { didSet { save() } }
    @Published var nightMode: NightMode { didSet { save() } }
    @Published var boardNotifications: Bool { didSet { save() } }
    @Published var speedAlerts: Bool { didSet { save() } }
    @Published var radarAlerts: Bool { didSet { save() } }

    private let d = UserDefaults.standard

    init() {
        dayBrightness = d.object(forKey: "set.dayBrightness") as? Double ?? 255
        nightBrightness = d.object(forKey: "set.nightBrightness") as? Double ?? 110
        nightMode = NightMode(rawValue: d.integer(forKey: "set.nightMode")) ?? .auto
        boardNotifications = d.object(forKey: "set.boardNotifications") as? Bool ?? true
        speedAlerts = d.object(forKey: "set.speedAlerts") as? Bool ?? true
        radarAlerts = d.object(forKey: "set.radarAlerts") as? Bool ?? true
    }

    private func save() {
        d.set(dayBrightness, forKey: "set.dayBrightness")
        d.set(nightBrightness, forKey: "set.nightBrightness")
        d.set(nightMode.rawValue, forKey: "set.nightMode")
        d.set(boardNotifications, forKey: "set.boardNotifications")
        d.set(speedAlerts, forKey: "set.speedAlerts")
        d.set(radarAlerts, forKey: "set.radarAlerts")
    }

    func isNight(at coordinate: CLLocationCoordinate2D?, date: Date = Date()) -> Bool {
        switch nightMode {
        case .always: return true
        case .never: return false
        case .auto:
            guard let c = coordinate, let sun = Solar.sunTimes(date: date, coordinate: c) else {
                let h = Calendar.current.component(.hour, from: date)
                return h >= 19 || h < 6
            }
            return date < sun.sunrise || date > sun.sunset
        }
    }

    func packet(coordinate: CLLocationCoordinate2D?) -> Data {
        let night = isNight(at: coordinate)
        let b = night ? nightBrightness : dayBrightness
        return Data([0x05, UInt8(clamping: Int(b.rounded())), night ? 1 : 0, boardNotifications ? 1 : 0])
    }
}

enum Solar {
    static func sunTimes(date: Date, coordinate c: CLLocationCoordinate2D) -> (sunrise: Date, sunset: Date)? {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "UTC")!
        guard let day = cal.ordinality(of: .day, in: .year, for: date) else { return nil }
        let startOfDay = cal.startOfDay(for: date)
        let n = Double(day)
        let lngHour = c.longitude / 15
        func event(rising: Bool) -> Date? {
            let t = n + ((rising ? 6 : 18) - lngHour) / 24
            let m = 0.9856 * t - 3.289
            var l = m + 1.916 * sin(m * .pi / 180) + 0.020 * sin(2 * m * .pi / 180) + 282.634
            l = l.truncatingRemainder(dividingBy: 360)
            if l < 0 { l += 360 }
            var ra = atan(0.91764 * tan(l * .pi / 180)) * 180 / .pi
            ra = ra.truncatingRemainder(dividingBy: 360)
            if ra < 0 { ra += 360 }
            let lQuad = floor(l / 90) * 90
            let raQuad = floor(ra / 90) * 90
            ra = (ra + lQuad - raQuad) / 15
            let sinDec = 0.39782 * sin(l * .pi / 180)
            let cosDec = cos(asin(sinDec))
            let zenith = 90.833 * .pi / 180
            let cosH = (cos(zenith) - sinDec * sin(c.latitude * .pi / 180)) / (cosDec * cos(c.latitude * .pi / 180))
            if cosH > 1 || cosH < -1 { return nil }
            var h = rising ? 360 - acos(cosH) * 180 / .pi : acos(cosH) * 180 / .pi
            h /= 15
            let localT = h + ra - 0.06571 * t - 6.622
            var ut = (localT - lngHour).truncatingRemainder(dividingBy: 24)
            if ut < 0 { ut += 24 }
            return startOfDay.addingTimeInterval(ut * 3600)
        }
        guard let rise = event(rising: true), let set = event(rising: false) else { return nil }
        return (rise, set)
    }
}
