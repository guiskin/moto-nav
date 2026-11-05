import SwiftUI
import MapKit

struct ContentView: View {
    @EnvironmentObject var ble: BLEManager
    @EnvironmentObject var location: LocationProvider
    @EnvironmentObject var engine: NavigationEngine
    @StateObject private var search = SearchModel()
    @State private var camera: MapCameraPosition = .userLocation(fallback: .automatic)
    @FocusState private var searchFocused: Bool

    var body: some View {
        ZStack(alignment: .top) {
            Map(position: $camera) {
                UserAnnotation()
                if let r = engine.route {
                    MapPolyline(r.polyline).stroke(.blue, lineWidth: 6)
                }
                if let d = engine.destination {
                    Marker(d.name ?? "Destino", coordinate: d.placemark.coordinate)
                }
            }
            .mapControls {
                MapUserLocationButton()
                MapCompass()
            }
            .ignoresSafeArea()

            VStack(spacing: 10) {
                statusPill
                switch engine.mode {
                case .idle: searchPanel
                case .preview: EmptyView()
                case .navigating: navPanel
                }
                Spacer()
                if engine.mode == .preview { previewPanel }
                if engine.mode == .navigating { bottomBar }
            }
            .padding()
        }
        .onChange(of: location.location) { _, loc in
            if let c = loc?.coordinate { search.setRegion(around: c) }
        }
        .onChange(of: engine.route) { _, r in
            guard let r = r, engine.mode != .navigating else { return }
            withAnimation {
                camera = .rect(r.polyline.boundingMapRect.insetBy(dx: -r.polyline.boundingMapRect.width * 0.15,
                                                                 dy: -r.polyline.boundingMapRect.height * 0.15))
            }
        }
        .onChange(of: engine.mode) { _, m in
            if m == .navigating {
                withAnimation { camera = .userLocation(followsHeading: true, fallback: .automatic) }
            }
        }
    }

    private var statusPill: some View {
        HStack(spacing: 8) {
            Circle().fill(ble.isConnected ? Color.green : Color.orange).frame(width: 10, height: 10)
            Text(ble.statusText).font(.subheadline.weight(.semibold))
            if engine.simulating {
                Text("· Simulação").font(.subheadline).foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.regularMaterial, in: Capsule())
    }

    private var searchPanel: some View {
        VStack(spacing: 0) {
            HStack {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Para onde?", text: $search.query)
                    .focused($searchFocused)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                if !search.query.isEmpty {
                    Button { search.clear() } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                    }
                }
            }
            .padding(12)

            if searchFocused && !search.results.isEmpty {
                Divider()
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(search.results.prefix(8).enumerated()), id: \.offset) { _, item in
                            Button {
                                select(item)
                            } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(item.title).foregroundStyle(.primary)
                                    if !item.subtitle.isEmpty {
                                        Text(item.subtitle).font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 12)
                                .padding(.vertical, 10)
                            }
                            Divider()
                        }
                    }
                }
                .frame(maxHeight: 320)
            }

            if engine.calculating {
                Divider()
                ProgressView("Calculando rota...").padding(12)
            }
            if let e = engine.errorText {
                Divider()
                Text(e).font(.footnote).foregroundStyle(.red).padding(12)
            }
        }
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var previewPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(engine.destination?.name ?? "Destino").font(.title3.bold())
            if let r = engine.route {
                Text("\(Format.distance(r.distance)) · \(Format.duration(r.expectedTravelTime))")
                    .foregroundStyle(.secondary)
            }
            Toggle("Evitar pedágios", isOn: $engine.avoidTolls)
            Toggle("Evitar rodovias", isOn: $engine.avoidHighways)
            HStack {
                Button("Cancelar") { engine.cancelPreview() }
                    .buttonStyle(.bordered)
                Button("Recalcular") {
                    if let d = engine.destination { Task { await engine.planRoute(to: d) } }
                }
                .buttonStyle(.bordered)
                Spacer()
                Button("Simular") { engine.start(simulate: true) }
                    .buttonStyle(.bordered)
                Button("Iniciar") { engine.start(simulate: false) }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var navPanel: some View {
        let s = engine.snapshot
        return HStack(spacing: 16) {
            Image(systemName: s.maneuver.symbol)
                .font(.system(size: 48, weight: .bold))
                .frame(width: 64)
            VStack(alignment: .leading, spacing: 4) {
                Text(s.rerouting ? "Recalculando..." : Format.distance(s.distanceToManeuver))
                    .font(.system(size: 34, weight: .bold))
                Text(s.street).font(.headline).lineLimit(2)
                if s.maneuver == .roundabout && s.exitNumber > 0 {
                    Text("\(s.exitNumber)ª saída").font(.subheadline).foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var bottomBar: some View {
        let s = engine.snapshot
        return HStack {
            VStack(alignment: .leading) {
                Text("\(Format.distance(s.remainingDistance)) · \(Format.duration(s.remainingTime))").font(.headline)
                Text("\(Int(s.speedKmh.rounded())) km/h").foregroundStyle(.secondary)
                if let p = ble.uploadProgress {
                    Text("Enviando mapa \(Int(p * 100))%").font(.caption).foregroundStyle(.orange)
                } else if let m = engine.mapStatus {
                    Text(m).font(.caption).foregroundStyle(.orange)
                }
            }
            Spacer()
            Button("Encerrar", role: .destructive) { engine.stop() }
                .buttonStyle(.borderedProminent)
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private func select(_ item: MKLocalSearchCompletion) {
        searchFocused = false
        Task {
            if let mapItem = await search.resolve(item) {
                search.clear()
                await engine.planRoute(to: mapItem)
            }
        }
    }
}

enum Format {
    static func distance(_ m: Double) -> String {
        if m < 1000 {
            let r = m < 100 ? (m / 5).rounded() * 5 : (m / 10).rounded() * 10
            return "\(Int(r)) m"
        }
        if m < 10_000 {
            return String(format: "%.1f km", m / 1000).replacingOccurrences(of: ".", with: ",")
        }
        return "\(Int((m / 1000).rounded())) km"
    }

    static func duration(_ t: TimeInterval) -> String {
        let min = Int((t / 60).rounded(.up))
        if min < 60 { return "\(min) min" }
        return String(format: "%dh%02d", min / 60, min % 60)
    }
}
