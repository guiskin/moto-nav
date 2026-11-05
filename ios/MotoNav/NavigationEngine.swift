import Foundation
import MapKit
import UIKit
import CoreLocation

struct NavSnapshot: Equatable {
    var maneuver: ManeuverCode = .none
    var exitNumber: UInt8 = 0
    var angle: Int16 = 0
    var distanceToManeuver: Double = 0
    var remainingDistance: Double = 0
    var remainingTime: TimeInterval = 0
    var speedKmh: Double = 0
    var instruction: String = ""
    var street: String = ""
    var rerouting = false
}

@MainActor
final class NavigationEngine: ObservableObject {
    enum Mode: Equatable { case idle, preview, navigating }

    @Published private(set) var mode: Mode = .idle
    @Published private(set) var route: MKRoute?
    @Published private(set) var destination: MKMapItem?
    @Published private(set) var snapshot = NavSnapshot()
    @Published private(set) var calculating = false
    @Published private(set) var errorText: String?
    @Published private(set) var simulating = false
    @Published var avoidTolls = false
    @Published var avoidHighways = false

    private var geometry: RouteGeometry?
    private weak var location: LocationProvider?
    private weak var ble: BLEManager?
    private var segmentHint: Int?
    private var offRouteCount = 0
    private var lastReroute = Date.distantPast
    private var simTimer: Timer?
    private var simAlong: Double = 0
    private let simSpeed: Double = 40 / 3.6
    private var heartbeat: Timer?
    private var lastFix: CLLocation?

    func attach(location: LocationProvider, ble: BLEManager) {
        self.location = location
        self.ble = ble
        location.onUpdate = { [weak self] loc in
            Task { @MainActor in self?.handle(location: loc, simulated: false) }
        }
        heartbeat?.invalidate()
        heartbeat = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pushToDevice() }
        }
    }

    func planRoute(to item: MKMapItem) async {
        destination = item
        errorText = nil
        guard let from = location?.location else {
            errorText = "Aguardando sua localização..."
            return
        }
        calculating = true
        defer { calculating = false }
        do {
            let r = try await calculate(from: from.coordinate, to: item)
            setRoute(r)
            mode = .preview
        } catch {
            errorText = "Não foi possível calcular a rota."
        }
    }

    private func calculate(from: CLLocationCoordinate2D, to item: MKMapItem) async throws -> MKRoute {
        let req = MKDirections.Request()
        req.source = MKMapItem(placemark: MKPlacemark(coordinate: from))
        req.destination = item
        req.transportType = .automobile
        req.requestsAlternateRoutes = false
        req.tollPreference = avoidTolls ? .avoid : .any
        req.highwayPreference = avoidHighways ? .avoid : .any
        let resp = try await MKDirections(request: req).calculate()
        guard let r = resp.routes.first else { throw URLError(.cannotFindHost) }
        return r
    }

    private func setRoute(_ r: MKRoute) {
        route = r
        geometry = RouteGeometry(route: r)
        segmentHint = nil
        offRouteCount = 0
    }

    func start(simulate: Bool) {
        guard geometry != nil else { return }
        mode = .navigating
        location?.setBackground(true)
        UIApplication.shared.isIdleTimerDisabled = true
        if simulate { startSimulation() }
        if let loc = simulate ? nil : location?.location { handle(location: loc, simulated: false) }
    }

    func stop() {
        stopSimulation()
        mode = .idle
        route = nil
        geometry = nil
        destination = nil
        snapshot = NavSnapshot()
        location?.setBackground(false)
        UIApplication.shared.isIdleTimerDisabled = false
        pushToDevice()
    }

    func cancelPreview() {
        if mode == .preview { stop() }
    }

    private func startSimulation() {
        stopSimulation()
        simulating = true
        simAlong = 0
        simTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.simulationTick() }
        }
    }

    private func stopSimulation() {
        simTimer?.invalidate()
        simTimer = nil
        simulating = false
    }

    private func simulationTick() {
        guard let g = geometry else { return }
        simAlong = min(simAlong + simSpeed, g.total)
        let c = g.point(at: simAlong)
        let loc = CLLocation(coordinate: c, altitude: 0, horizontalAccuracy: 5, verticalAccuracy: 5,
                             course: g.bearing(at: simAlong), speed: simAlong >= g.total ? 0 : simSpeed, timestamp: Date())
        handle(location: loc, simulated: true)
        if simAlong >= g.total { stopSimulation() }
    }

    private func handle(location loc: CLLocation, simulated: Bool) {
        if simulating && !simulated { return }
        lastFix = loc
        guard mode == .navigating, let g = geometry else { return }
        guard let proj = g.project(loc.coordinate, near: segmentHint) else { return }

        if proj.offRoute > 45 && loc.horizontalAccuracy < 40 && !simulated {
            offRouteCount += 1
        } else {
            offRouteCount = 0
            segmentHint = proj.segment
        }
        if offRouteCount >= 3 {
            reroute(from: loc)
        }

        var s = NavSnapshot()
        s.speedKmh = max(0, loc.speed) * 3.6
        s.rerouting = snapshot.rerouting
        let along = proj.along
        s.remainingDistance = max(0, g.total - along)
        if let r = route, g.total > 0 {
            s.remainingTime = r.expectedTravelTime * s.remainingDistance / g.total
        }
        if s.remainingDistance < 25 {
            s.maneuver = .arrive
            s.distanceToManeuver = 0
            s.instruction = "Você chegou"
            s.street = destination?.name ?? "Destino"
        } else if let k = g.nextStepIndex(after: along) {
            let m = g.maneuver(forStep: k)
            s.maneuver = m.code
            s.exitNumber = m.exitNumber
            s.angle = m.angle
            s.distanceToManeuver = max(0, g.stepStart[k] - along)
            s.instruction = g.instructions[k]
            s.street = m.code == .arrive ? (destination?.name ?? "Destino") : RouteGeometry.street(from: g.instructions[k])
            if m.code == .arrive { s.distanceToManeuver = s.remainingDistance }
        } else {
            s.maneuver = .arrive
            s.distanceToManeuver = s.remainingDistance
            s.street = destination?.name ?? "Destino"
        }
        snapshot = s
        pushToDevice()
    }

    private func reroute(from loc: CLLocation) {
        guard Date().timeIntervalSince(lastReroute) > 8, let dest = destination else { return }
        lastReroute = Date()
        offRouteCount = 0
        snapshot.rerouting = true
        pushToDevice()
        Task {
            do {
                let r = try await calculate(from: loc.coordinate, to: dest)
                setRoute(r)
            } catch {}
            snapshot.rerouting = false
            if let fix = lastFix { handle(location: fix, simulated: simulating) }
        }
    }

    private func pushToDevice() {
        guard let ble = ble else { return }
        ble.send(packet(maxLength: ble.maxPayload))
    }

    func packet(maxLength: Int) -> Data {
        let active = mode == .navigating
        let s = snapshot
        var d = Data()
        d.append(1)
        d.append(active ? 1 : 0)
        d.append(s.maneuver.rawValue)
        d.append(s.exitNumber)
        appendLE(&d, UInt16(bitPattern: s.angle))
        appendLE(&d, UInt32(clamping: Int(s.distanceToManeuver.rounded())))
        appendLE(&d, UInt32(clamping: Int(s.remainingDistance.rounded())))
        appendLE(&d, UInt16(clamping: Int((s.remainingTime / 60).rounded(.up))))
        d.append(UInt8(clamping: Int(s.speedKmh.rounded())))
        d.append(s.rerouting ? 1 : 0)
        let room = max(0, min(120, maxLength - d.count))
        d.append(Self.utf8Prefix(s.street, maxBytes: room))
        return d
    }

    private func appendLE<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
        withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) }
    }

    static func utf8Prefix(_ s: String, maxBytes: Int) -> Data {
        var out = Data()
        for ch in s {
            let bytes = Data(String(ch).utf8)
            if out.count + bytes.count > maxBytes { break }
            out.append(bytes)
        }
        return out
    }
}
