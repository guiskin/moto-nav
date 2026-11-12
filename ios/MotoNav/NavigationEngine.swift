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
    var curveDistance: Double?
    var curveAngle: Double = 0
    var limit: Int?
    var roadName: String?
    var radarDistance: Double?
    var tripDistance: Double = 0
}

@MainActor
final class NavigationEngine: ObservableObject {
    enum Mode: Equatable { case idle, preview, navigating, cruising }

    var isActive: Bool { mode == .navigating || mode == .cruising }

    @Published private(set) var mode: Mode = .idle
    @Published private(set) var route: MKRoute?
    @Published private(set) var destination: MKMapItem?
    @Published private(set) var snapshot = NavSnapshot()
    @Published private(set) var calculating = false
    @Published private(set) var errorText: String?
    @Published private(set) var simulating = false
    @Published private(set) var mapStatus: String?
    @Published var avoidTolls = false
    @Published var avoidHighways = false
    @Published var voiceEnabled: Bool = VoiceGuide().enabled {
        didSet { voice.enabled = voiceEnabled }
    }
    private let voice = VoiceGuide()

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
    private var geometryVersion = 0
    private var session: UInt16 = 0
    private var projection: LocalProjection?
    private var tiles: TileManager?
    private var routeBlob: RouteBlob?
    private var routeBlobVersion = -1
    private var routeOnBoardVersion = -1
    private weak var settings: SettingsStore?
    private weak var trips: TripStore?
    private var trip: TripRecorder?
    private var overSince: Date?
    private var lastOverAlert = Date.distantPast
    private var lastSettingsPush = Date.distantPast

    func attach(location: LocationProvider, ble: BLEManager, settings: SettingsStore, trips: TripStore) {
        self.location = location
        self.ble = ble
        self.settings = settings
        self.trips = trips
        ble.onReady = { [weak self] in
            Task { @MainActor in self?.boardReconnected() }
        }
        location.onUpdate = { [weak self] loc in
            Task { @MainActor in self?.handle(location: loc, simulated: false) }
        }
        heartbeat?.invalidate()
        heartbeat = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self = self else { return }
                self.pushToDevice()
                self.resendPosition()
                if Date().timeIntervalSince(self.lastSettingsPush) > 30 { self.pushSettings() }
            }
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
        geometryVersion += 1
        segmentHint = nil
        offRouteCount = 0
        if mode == .navigating { uploadRoute() }
    }

    func pushSettings() {
        guard let ble = ble, let settings = settings else { return }
        lastSettingsPush = Date()
        ble.sendPosition(settings.packet(coordinate: location?.location?.coordinate))
    }

    func startCruise() {
        guard let loc = location?.location else {
            errorText = "Aguardando sua localização..."
            return
        }
        geometry = nil
        route = nil
        destination = nil
        snapshot = NavSnapshot()
        mode = .cruising
        voice.reset()
        trip = TripRecorder(title: "Passeio")
        startSession(origin: loc.coordinate)
        location?.setBackground(true)
        UIApplication.shared.isIdleTimerDisabled = true
        handle(location: loc, simulated: false)
    }

    private func startSession(origin explicitOrigin: CLLocationCoordinate2D? = nil) {
        session = UInt16.random(in: 1...65_000)
        let origin = explicitOrigin ?? geometry?.coords.first ?? location?.location?.coordinate ?? CLLocationCoordinate2D(latitude: 0, longitude: 0)
        let proj = LocalProjection(origin: origin)
        projection = proj
        let tm = TileManager(session: session, projection: proj)
        tm.upload = { [weak self] data in self?.ble?.sendMap(blob: data, priority: false) }
        tm.onStatus = { [weak self] text in self?.mapStatus = text }
        tiles = tm
        if geometry != nil { uploadRoute() }
        if let fix = location?.location {
            updateTiles(fix)
        } else if let first = geometry?.coords.first {
            updateTiles(CLLocation(latitude: first.latitude, longitude: first.longitude))
        }
    }

    private func uploadRoute() {
        guard let g = geometry, let proj = projection else { return }
        routeBlob = MapBuilder.routeBlob(session: session, projection: proj, geometry: g)
        routeBlobVersion = geometryVersion
        sendRoute()
    }

    private func sendRoute() {
        guard mode == .navigating, let rb = routeBlob, let ble = ble, ble.isConnected else { return }
        let version = routeBlobVersion
        ble.sendMap(blob: rb.data, priority: true) { [weak self] in
            Task { @MainActor in
                guard let self = self else { return }
                self.routeOnBoardVersion = version
                self.resendPosition()
            }
        }
    }

    private func boardReconnected() {
        pushSettings()
        guard isActive else { return }
        routeOnBoardVersion = -1
        sendRoute()
        tiles?.resetSent()
        if let fix = lastFix { updateTiles(fix) }
    }

    private func updateTiles(_ loc: CLLocation) {
        guard let tm = tiles, let proj = projection else { return }
        let p = proj.project(loc.coordinate)
        var ahead: [(x: Double, y: Double)] = []
        if let g = geometry, let pr = g.project(loc.coordinate, near: segmentHint), pr.offRoute < 200 {
            var d = pr.along + 400
            while d <= min(pr.along + 3_000, g.total) {
                let q = proj.project(g.point(at: d))
                ahead.append((x: q.x, y: q.y))
                d += 400
            }
        } else if loc.course >= 0 && loc.speed > 2 {
            let h = loc.course * .pi / 180
            for d in stride(from: 400.0, through: 1_600, by: 400) {
                ahead.append((x: p.x + sin(h) * d, y: p.y + cos(h) * d))
            }
        }
        tm.update(position: (x: p.x, y: p.y), ahead: ahead)
    }

    private func resendPosition() {
        guard isActive, let loc = lastFix else { return }
        sendPosition(loc, projection: geometry?.project(loc.coordinate, near: segmentHint), geometry: geometry)
    }

    private func sendPosition(_ loc: CLLocation, projection proj: RouteGeometry.Projection?, geometry g: RouteGeometry?) {
        guard let ble = ble, let local = projection else { return }
        let p = local.project(loc.coordinate)
        var along: Int32 = -1
        if let g = g, let proj = proj, let rb = routeBlob, routeOnBoardVersion == geometryVersion, routeBlobVersion == geometryVersion, proj.offRoute < 30 {
            let i = proj.segment
            let cum = rb.localCum
            if i + 1 < cum.count && i + 1 < g.cum.count {
                let segLen = g.cum[i + 1] - g.cum[i]
                let t = segLen > 0 ? min(max((proj.along - g.cum[i]) / segLen, 0), 1) : 0
                along = Int32(clamping: Int(((cum[i] + (cum[i + 1] - cum[i]) * t) * 10).rounded()))
            }
        }
        var d = Data([0x02])
        appendLE(&d, session)
        appendLE(&d, Int32(clamping: Int((p.x * 10).rounded())))
        appendLE(&d, Int32(clamping: Int((p.y * 10).rounded())))
        let heading: UInt16 = loc.course >= 0 ? UInt16(clamping: Int((loc.course * 100).rounded()) % 36_000) : 0xFFFF
        appendLE(&d, heading)
        appendLE(&d, UInt16(clamping: Int((max(0, loc.speed) * 100).rounded())))
        appendLE(&d, along)
        ble.sendPosition(d)
    }

    func speak(_ text: String) {
        voice.say(text)
    }

    func start(simulate: Bool) {
        guard geometry != nil else { return }
        mode = .navigating
        voice.reset()
        trip = simulate ? nil : TripRecorder(title: destination?.name ?? "Viagem")
        startSession()
        location?.setBackground(true)
        UIApplication.shared.isIdleTimerDisabled = true
        if simulate { startSimulation() }
        if let loc = simulate ? nil : location?.location { handle(location: loc, simulated: false) }
    }

    func stop() {
        stopSimulation()
        if let t = trip?.finish() { trips?.add(t) }
        trip = nil
        tiles = nil
        projection = nil
        routeBlob = nil
        routeBlobVersion = -1
        routeOnBoardVersion = -1
        mapStatus = nil
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
        guard isActive else { return }
        if !simulated { trip?.add(loc) }
        if mode == .cruising {
            var s = NavSnapshot()
            s.speedKmh = max(0, loc.speed) * 3.6
            roadAndRadar(loc, &s)
            s.street = s.roadName ?? ""
            s.tripDistance = trip?.distance ?? 0
            snapshot = s
            pushToDevice()
            sendPosition(loc, projection: nil, geometry: nil)
            sendStatus()
            updateTiles(loc)
            return
        }
        guard let g = geometry else { return }
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
            announce(step: k, maneuver: m, distance: max(0, g.stepStart[k] - along), loc: loc)
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
        if s.remainingDistance < 25 {
            voice.say("Você chegou ao destino", once: "arrive-\(geometryVersion)")
        }
        if proj.offRoute < 45, let c = g.nextCurve(after: along, within: 300) {
            let dist = max(0, c.curve.along - along)
            s.curveDistance = dist
            s.curveAngle = c.curve.angle
            if dist <= triggerDistance(loc) {
                let side = c.curve.angle > 0 ? "à direita" : "à esquerda"
                let sharp = abs(c.curve.angle) >= 90 ? "curva fechada" : "curva"
                voice.say("Em \(shortDistance(dist)), \(sharp) \(side)", once: "curve-\(geometryVersion)-\(c.index)")
            }
        }
        roadAndRadar(loc, &s)
        s.tripDistance = trip?.distance ?? 0
        snapshot = s
        pushToDevice()
        sendPosition(loc, projection: proj, geometry: g)
        sendAlert()
        sendStatus()
        updateTiles(loc)
    }

    private func roadAndRadar(_ loc: CLLocation, _ s: inout NavSnapshot) {
        guard let tm = tiles, let proj = projection else { return }
        let p = proj.project(loc.coordinate)
        let course: Double? = loc.course >= 0 && loc.speed > 1.5 ? loc.course : nil
        let info = tm.roadInfo(at: (x: p.x, y: p.y), course: course)
        s.limit = info.limit
        s.roadName = info.name
        if let cam = tm.nextCamera(at: (x: p.x, y: p.y), course: course) {
            let alertDist = max(300, min(600, loc.speed * 12))
            if cam.distance <= alertDist {
                s.radarDistance = cam.distance
                if settings?.radarAlerts ?? true, cam.distance > 30 {
                    voice.say("Radar à frente", once: "radar-\(session)-\(cam.key)")
                }
            }
        }
        if let lim = s.limit, settings?.speedAlerts ?? true, s.speedKmh > Double(lim) + 7 {
            if overSince == nil { overSince = Date() }
            if let since = overSince, Date().timeIntervalSince(since) > 3, Date().timeIntervalSince(lastOverAlert) > 60 {
                lastOverAlert = Date()
                voice.say("Atenção, limite de \(lim)")
            }
        } else {
            overSince = nil
        }
    }

    private func sendStatus() {
        guard let ble = ble else { return }
        var d = Data([0x04])
        d.append(UInt8(clamping: snapshot.limit ?? 0))
        appendLE(&d, snapshot.radarDistance.map { UInt16(clamping: Int($0.rounded())) } ?? UInt16(0xFFFF))
        d.append(0)
        d.append(Self.utf8Prefix(snapshot.roadName ?? "", maxBytes: 60))
        ble.sendPosition(d)
    }

    func etaShareText() -> String? {
        guard mode == .navigating, let dest = destination else { return nil }
        let arrival = Date().addingTimeInterval(snapshot.remainingTime)
        let f = DateFormatter()
        f.locale = Locale(identifier: "pt_BR")
        f.dateFormat = "HH:mm"
        let c = dest.placemark.coordinate
        let name = (dest.name ?? "destino").addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? ""
        let link = String(format: "https://maps.apple.com/?ll=%.6f,%.6f&q=%@", c.latitude, c.longitude, name)
        return "Estou indo de moto para \(dest.name ?? "o destino"). Chego por volta das \(f.string(from: arrival)) (\(Format.duration(snapshot.remainingTime))). \(link)"
    }

    private func triggerDistance(_ loc: CLLocation) -> Double {
        max(50, max(0, loc.speed) * 4)
    }

    private func shortDistance(_ m: Double) -> String {
        let r = max(10, (m / 10).rounded() * 10)
        return "\(Int(r)) metros"
    }

    private func announce(step k: Int, maneuver m: Maneuver, distance: Double, loc: CLLocation) {
        let action: String
        switch m.code {
        case .right, .sharpRight: action = "vire à direita"
        case .left, .sharpLeft: action = "vire à esquerda"
        case .slightRight, .keepRight: action = "mantenha à direita"
        case .slightLeft, .keepLeft: action = "mantenha à esquerda"
        case .uturn: action = "faça o retorno"
        case .roundabout: action = m.exitNumber > 0 ? "na rotatória, \(m.exitNumber)ª saída" : "entre na rotatória"
        default: return
        }
        guard distance <= triggerDistance(loc) else { return }
        voice.say("Em \(shortDistance(distance)), \(action)", once: "turn-\(geometryVersion)-\(k)")
    }

    private func sendAlert() {
        guard let ble = ble else { return }
        var d = Data([0x03])
        if let dist = snapshot.curveDistance {
            d.append(1)
            appendLE(&d, Int16(clamping: Int(snapshot.curveAngle.rounded())))
            appendLE(&d, UInt16(clamping: Int(dist.rounded())))
        } else {
            d.append(0)
            appendLE(&d, Int16(0))
            appendLE(&d, UInt16(0))
        }
        ble.sendPosition(d)
    }

    private func reroute(from loc: CLLocation) {
        guard Date().timeIntervalSince(lastReroute) > 8, let dest = destination else { return }
        lastReroute = Date()
        offRouteCount = 0
        snapshot.rerouting = true
        voice.say("Recalculando rota")
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
        let active = isActive
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
        d.append((s.rerouting ? 1 : 0) | (mode == .cruising ? 2 : 0))
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
