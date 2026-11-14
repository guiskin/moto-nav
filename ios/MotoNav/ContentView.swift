import SwiftUI
import MapKit

struct ContentView: View {
    @EnvironmentObject var ble: BLEManager
    @EnvironmentObject var location: LocationProvider
    @EnvironmentObject var engine: NavigationEngine
    @EnvironmentObject var places: PlacesStore
    @EnvironmentObject var settings: SettingsStore
    @EnvironmentObject var trips: TripStore
    @EnvironmentObject var playlists: PlaylistStore
    @EnvironmentObject var voiceCenter: VoiceCommandCenter
    @State private var showSettings = false
    @State private var showTrips = false
    @State private var showFavorites = false
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
                HStack(spacing: 8) {
                    statusPill
                    Spacer()
                    if !engine.isActive {
                        circleButton("star.fill") { showFavorites = true }
                        circleButton("list.bullet") { showTrips = true }
                    }
                    circleButton("gearshape.fill") { showSettings = true }
                }
                switch engine.mode {
                case .idle: searchPanel
                case .preview: EmptyView()
                case .navigating: navPanel
                case .cruising: cruisePanel
                }
                Spacer()
                if engine.mode == .preview { previewPanel }
                if engine.isActive { bottomBar }
            }
            .padding()
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
                .environmentObject(playlists)
                .environmentObject(voiceCenter)
                .environmentObject(settings)
                .environmentObject(ble)
                .environmentObject(engine)
        }
        .sheet(isPresented: $showFavorites) {
            FavoritesView { item in
                showFavorites = false
                go(to: item)
            }
            .environmentObject(places)
            .environmentObject(location)
        }
        .sheet(isPresented: $showTrips) {
            TripsView().environmentObject(trips)
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
            if m == .navigating || m == .cruising {
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

            if !searchFocused && search.query.isEmpty {
                Divider()
                Button {
                    engine.startCruise()
                } label: {
                    Label("Passeio sem rota", systemImage: "road.lanes")
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)
                }
            }

            if searchFocused && search.query.isEmpty && (!places.favorites.isEmpty || !places.recents.isEmpty) {
                Divider()
                savedList
            }

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

    @State private var showFavName = false
    @State private var favName = ""

    private var previewPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(engine.destination?.name ?? "Destino").font(.title3.bold())
                Spacer()
                Button {
                    if places.isFavorite(engine.destination) {
                        places.toggleFavorite(engine.destination)
                    } else {
                        favName = engine.destination?.name ?? ""
                        showFavName = true
                    }
                } label: {
                    Image(systemName: places.isFavorite(engine.destination) ? "star.fill" : "star")
                        .font(.title2)
                        .foregroundStyle(.yellow)
                }
            }
            if let r = engine.route {
                Text("\(Format.distance(r.distance)) · \(Format.duration(r.expectedTravelTime))")
                    .foregroundStyle(.secondary)
            }
            Toggle("Evitar pedágios", isOn: $engine.avoidTolls)
            Toggle("Evitar rodovias", isOn: $engine.avoidHighways)
            Toggle("Avisos por voz", isOn: $engine.voiceEnabled)
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
        .alert("Nome do favorito", isPresented: $showFavName) {
            TextField("Ex.: Casa, Trabalho", text: $favName)
            Button("Salvar") { places.toggleFavorite(engine.destination, name: favName) }
            Button("Cancelar", role: .cancel) {}
        }
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
                if let l = s.lanes { LaneStrip(info: l) }
                limitRadarLine(s)
                if let cd = s.curveDistance {
                    Label("\(abs(s.curveAngle) >= 90 ? "Curva acentuada" : "Curva") \(s.curveAngle > 0 ? "à direita" : "à esquerda") em \(Format.distance(cd))",
                          systemImage: "exclamationmark.triangle.fill")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.yellow)
                }
            }
            Spacer()
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private func circleButton(_ icon: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.headline)
                .frame(width: 40, height: 40)
                .background(.regularMaterial, in: Circle())
        }
        .foregroundStyle(.primary)
    }

    @ViewBuilder
    private func limitRadarLine(_ s: NavSnapshot) -> some View {
        if s.limit != nil || s.radarDistance != nil {
            HStack(spacing: 12) {
                if let l = s.limit {
                    Text("Limite \(l)")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(s.speedKmh > Double(l) + 3 ? .red : .primary)
                }
                if let r = s.radarDistance {
                    Label("Radar \(Format.distance(r))", systemImage: "camera.fill")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.orange)
                }
            }
        }
    }

    private var cruisePanel: some View {
        let s = engine.snapshot
        return VStack(alignment: .leading, spacing: 6) {
            Text(s.roadName ?? "Passeio").font(.title3.bold()).lineLimit(1)
            limitRadarLine(s)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var bottomBar: some View {
        let s = engine.snapshot
        return HStack {
            VStack(alignment: .leading) {
                if engine.mode == .cruising {
                    Text("Passeio · \(Format.distance(s.tripDistance))").font(.headline)
                } else {
                    Text("\(Format.distance(s.remainingDistance)) · \(Format.duration(s.remainingTime))").font(.headline)
                }
                Text("\(Int(s.speedKmh.rounded())) km/h").foregroundStyle(.secondary)
                if let p = ble.uploadProgress {
                    Text("Enviando mapa \(Int(p * 100))%").font(.caption).foregroundStyle(.orange)
                } else if let m = engine.mapStatus {
                    Text(m).font(.caption).foregroundStyle(.orange)
                }
            }
            Spacer()
            if let text = engine.etaShareText() {
                ShareLink(item: text) {
                    Image(systemName: "square.and.arrow.up")
                        .font(.headline)
                        .frame(width: 40, height: 40)
                }
                .buttonStyle(.bordered)
            }
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
                go(to: mapItem)
            }
        }
    }

    private func go(to item: MKMapItem) {
        searchFocused = false
        places.addRecent(item)
        Task { await engine.planRoute(to: item) }
    }

    private var savedList: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                if !places.favorites.isEmpty {
                    sectionHeader("Favoritos")
                    ForEach(places.favorites) { p in
                        savedRow(p, icon: "star.fill", tint: .yellow) { places.removeFavorite(p) }
                    }
                }
                if !places.recents.isEmpty {
                    HStack {
                        sectionHeader("Recentes")
                        Spacer()
                        Button("Limpar") { places.clearRecents() }
                            .font(.caption)
                            .padding(.trailing, 12)
                    }
                    ForEach(places.recents) { p in
                        savedRow(p, icon: "clock", tint: .secondary) { places.removeRecent(p) }
                    }
                }
            }
        }
        .frame(maxHeight: 360)
    }

    private func sectionHeader(_ title: String) -> some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .padding(.horizontal, 12)
            .padding(.top, 10)
            .padding(.bottom, 4)
    }

    private func savedRow(_ p: SavedPlace, icon: String, tint: Color, remove: @escaping () -> Void) -> some View {
        Button {
            go(to: p.mapItem)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: icon).foregroundStyle(tint).frame(width: 20)
                VStack(alignment: .leading, spacing: 2) {
                    Text(p.name).foregroundStyle(.primary).lineLimit(1)
                    if !p.subtitle.isEmpty {
                        Text(p.subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
        }
        .contextMenu {
            Button("Remover", role: .destructive, action: remove)
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


struct LaneStrip: View {
    let info: LaneInfo

    var body: some View {
        HStack(spacing: 4) {
            ForEach(0..<info.dirs.count, id: \.self) { i in
                let ok = info.valid & UInt8(1 << i) != 0
                Image(systemName: symbol(info.dirs[i]))
                    .font(.system(size: 18, weight: .bold))
                    .frame(width: 26, height: 32)
                    .foregroundStyle(ok ? Color.white : Color.gray.opacity(0.7))
                    .background(ok ? Color.blue : Color.gray.opacity(0.2), in: RoundedRectangle(cornerRadius: 5))
            }
        }
    }

    private func symbol(_ d: UInt8) -> String {
        let r = d & LaneInfo.right != 0, l = d & LaneInfo.left != 0, t = d & LaneInfo.through != 0
        if d & LaneInfo.reverse != 0 && !r && !t { return "arrow.uturn.left" }
        if t && r { return "arrow.up.right" }
        if t && l { return "arrow.up.left" }
        if r { return "arrow.turn.up.right" }
        if l { return "arrow.turn.up.left" }
        return "arrow.up"
    }
}
