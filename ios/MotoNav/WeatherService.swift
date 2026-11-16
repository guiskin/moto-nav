import Foundation
import CoreLocation

struct WeatherInfo: Equatable {
    var temp: Int
    var kind: UInt8
    var rainMin: Int
}

enum WeatherService {
    private struct R: Decodable {
        struct Cur: Decodable { let temperature_2m: Double?; let precipitation: Double?; let weather_code: Int? }
        struct Hr: Decodable { let precipitation: [Double?]?; let precipitation_probability: [Double?]? }
        let current: Cur?
        let hourly: Hr?
    }

    private static func load(_ c: CLLocationCoordinate2D) async -> R? {
        let u = "https://api.open-meteo.com/v1/forecast?latitude=\(c.latitude)&longitude=\(c.longitude)&current=temperature_2m,precipitation,weather_code&hourly=precipitation,precipitation_probability&forecast_hours=3&timezone=auto"
        guard let url = URL(string: u), let (d, _) = try? await URLSession.shared.data(from: url) else { return nil }
        return try? JSONDecoder().decode(R.self, from: d)
    }

    static func fetch(here: CLLocationCoordinate2D, ahead: CLLocationCoordinate2D?, aheadMinutes: Int) async -> WeatherInfo? {
        guard let h = await load(here) else { return nil }
        let temp = Int((h.current?.temperature_2m ?? 0).rounded())
        let nowRain = (h.current?.precipitation ?? 0) >= 0.2
        var stormCode = (h.current?.weather_code ?? 0) >= 95
        var kind: UInt8 = nowRain ? 2 : 0
        var rainMin = 0
        if let a = ahead, let r = await load(a) {
            if (r.current?.weather_code ?? 0) >= 95 { stormCode = true }
            let p = r.hourly?.precipitation ?? []
            let pr = r.hourly?.precipitation_probability ?? []
            let wet = (p.prefix(2).contains { ($0 ?? 0) >= 0.3 }) || (pr.prefix(2).contains { ($0 ?? 0) >= 70 })
            if wet && !nowRain { kind = 1; rainMin = max(1, aheadMinutes) }
        }
        if stormCode { kind = 3 }
        return WeatherInfo(temp: temp, kind: kind, rainMin: rainMin)
    }
}
