import Foundation
import Speech
import AVFoundation
import UIKit
import MapKit

struct ImaAdpcm {
    static let idxTable: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
    static let stepTable: [Int] = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]

    static func decode(_ bytes: ArraySlice<UInt8>, predictor: Int, index: Int) -> [Int16] {
        var pred = predictor
        var idx = min(88, max(0, index))
        var out: [Int16] = []
        out.reserveCapacity(bytes.count * 2)
        for b in bytes {
            for code in [Int(b & 0x0F), Int(b >> 4)] {
                let step = stepTable[idx]
                var vp = step >> 3
                if code & 4 != 0 { vp += step }
                if code & 2 != 0 { vp += step >> 1 }
                if code & 1 != 0 { vp += step >> 2 }
                pred += (code & 8) != 0 ? -vp : vp
                pred = min(32767, max(-32768, pred))
                idx = min(88, max(0, idx + idxTable[code]))
                out.append(Int16(pred))
            }
        }
        return out
    }
}

final class FinishFlag {
    var done = false
}

final class VoiceReceiver {
    private var session: UInt8 = 0
    private var samples: [Int16] = []
    private var blocks = 0
    private var rate = 16000
    private var active = false
    var onComplete: (([Int16], Int) -> Void)?
    static let blockSamples = 200

    static func isAudio(_ d: Data) -> Bool {
        guard let f = d.first else { return false }
        return f >= 0xB0 && f <= 0xB2
    }

    func handle(_ d: Data) {
        let b = [UInt8](d)
        guard b.count >= 4 else { return }
        switch b[0] {
        case 0xB0:
            guard b.count >= 6 else { return }
            session = b[1]
            rate = Int(b[2]) | (Int(b[3]) << 8)
            blocks = Int(b[4]) | (Int(b[5]) << 8)
            samples = [Int16](repeating: 0, count: blocks * Self.blockSamples)
            active = true
        case 0xB1:
            guard active, b[1] == session, b.count >= 8 else { return }
            let seq = Int(b[2]) | (Int(b[3]) << 8)
            guard seq < blocks else { return }
            var pred = Int(b[4]) | (Int(b[5]) << 8)
            if pred > 32767 { pred -= 65536 }
            let dec = ImaAdpcm.decode(b[7...], predictor: pred, index: Int(b[6]))
            let start = seq * Self.blockSamples
            for (i, s) in dec.enumerated() where start + i < samples.count { samples[start + i] = s }
        case 0xB2:
            guard active, b[1] == session else { return }
            active = false
            let s = samples
            samples = []
            onComplete?(s, rate)
        default:
            break
        }
    }
}

struct SpotifyPlaylist: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var uri: String
}

final class PlaylistStore: ObservableObject {
    @Published private(set) var items: [SpotifyPlaylist] = []
    private let key = "motonav.playlists"

    init() {
        if let d = UserDefaults.standard.data(forKey: key),
           let v = try? JSONDecoder().decode([SpotifyPlaylist].self, from: d) { items = v }
    }

    static func uri(from link: String) -> String? {
        let t = link.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.hasPrefix("spotify:playlist:") { return t }
        if let r = t.range(of: "playlist/") {
            let rest = t[r.upperBound...]
            let id = rest.prefix { $0.isLetter || $0.isNumber }
            if !id.isEmpty { return "spotify:playlist:\(id)" }
        }
        return nil
    }

    func add(name: String, link: String) -> Bool {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty, let u = Self.uri(from: link) else { return false }
        items.append(SpotifyPlaylist(name: n, uri: u))
        save()
        return true
    }

    func remove(_ p: SpotifyPlaylist) {
        items.removeAll { $0.id == p.id }
        save()
    }

    private func save() {
        if let d = try? JSONEncoder().encode(items) { UserDefaults.standard.set(d, forKey: key) }
    }
}

enum VoiceIntent: Equatable {
    case navigate(String)
    case playlist(String)
    case stop
    case none
}

enum VoiceParser {
    static func normalize(_ s: String) -> String {
        let f = s.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: Locale(identifier: "pt_BR")).lowercased()
        let cleaned = f.map { $0.isLetter || $0.isNumber ? $0 : " " }
        return String(cleaned).split(separator: " ").joined(separator: " ")
    }

    static func stripArticles(_ s: String) -> String {
        var w = s.split(separator: " ").map(String.init)
        while let f = w.first, ["a", "o", "as", "os", "na", "no", "da", "do", "minha", "meu", "ao", "para", "pra"].contains(f), w.count > 1 { w.removeFirst() }
        return w.joined(separator: " ")
    }

    static func parse(_ raw: String, favorites: [String]) -> VoiceIntent {
        let t = normalize(raw)
        if t.isEmpty { return .none }
        let words = t.split(separator: " ").map(String.init)
        if let i = words.firstIndex(where: { $0 == "playlist" || $0 == "playlists" }) {
            let target = stripArticles(words[(i + 1)...].joined(separator: " "))
            return target.isEmpty ? .none : .playlist(target)
        }
        if let f = words.first, ["tocar", "toca", "toque", "coloca", "colocar", "poe"].contains(f) {
            var target = words.dropFirst().joined(separator: " ")
            target = target.replacingOccurrences(of: " no spotify", with: "")
            target = stripArticles(target)
            return target.isEmpty ? .none : .playlist(target)
        }
        if (t.contains("cancelar") || t.contains("encerrar") || t.contains("parar") || t.contains("finalizar")) &&
            (t.contains("rota") || t.contains("navegacao") || t.contains("viagem") || t.contains("gps")) {
            return .stop
        }
        let triggers = ["para", "pra", "ate"]
        let starts = ["vamos", "vou", "ir", "va", "leve", "levar", "navegar", "navega", "rota", "destino", "iniciar", "inicia", "me", "bora", "vai"]
        if let ti = words.firstIndex(where: { triggers.contains($0) }), words[..<ti].contains(where: { starts.contains($0) }), ti + 1 < words.count {
            return .navigate(stripArticles(words[(ti + 1)...].joined(separator: " ")))
        }
        for f in favorites {
            let n = normalize(f)
            if !n.isEmpty && t.contains(n) { return .navigate(n) }
        }
        return .none
    }

    static func distance(_ a: String, _ b: String) -> Int {
        let x = Array(a), y = Array(b)
        if x.isEmpty { return y.count }
        if y.isEmpty { return x.count }
        var prev = Array(0...y.count)
        for i in 1...x.count {
            var cur = [i] + [Int](repeating: 0, count: y.count)
            for j in 1...y.count {
                cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1))
            }
            prev = cur
        }
        return prev[y.count]
    }

    static func bestMatch(_ target: String, in names: [String]) -> Int? {
        let t = normalize(target)
        var best: (Int, Int)?
        for (i, name) in names.enumerated() {
            let n = normalize(name)
            var score = 0
            if n == t { score = 100 }
            else if t.contains(n) || n.contains(t) { score = 60 + min(n.count, t.count) }
            else if distance(n, t) <= max(1, min(n.count, t.count) / 4) { score = 40 }
            if score > 0 && (best == nil || score > best!.1) { best = (i, score) }
        }
        return best?.0
    }
}

@MainActor
final class VoiceCommandCenter: ObservableObject {
    @Published private(set) var lastHeard = ""
    private weak var ble: BLEManager?
    private weak var engine: NavigationEngine?
    private weak var places: PlacesStore?
    private weak var playlists: PlaylistStore?
    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "pt-BR"))
    private var task: SFSpeechRecognitionTask?

    func attach(ble: BLEManager, engine: NavigationEngine, places: PlacesStore, playlists: PlaylistStore) {
        self.ble = ble
        self.engine = engine
        self.places = places
        self.playlists = playlists
        ble.onVoice = { [weak self] samples, rate in
            Task { @MainActor in self?.process(samples, rate: rate) }
        }
        SFSpeechRecognizer.requestAuthorization { _ in }
    }

    private func reply(_ ok: Bool, _ text: String, speak: Bool = true) {
        var d = Data([0x06, ok ? 1 : 0])
        d.append(NavigationEngine.utf8Prefix(text, maxBytes: 80))
        ble?.sendPosition(d)
        if speak { engine?.speak(text) }
    }

    private func process(_ samples: [Int16], rate: Int) {
        guard let recognizer = recognizer, recognizer.isAvailable else {
            reply(false, "Reconhecimento de voz indisponível")
            return
        }
        guard SFSpeechRecognizer.authorizationStatus() == .authorized else {
            reply(false, "Permita o reconhecimento de voz nos Ajustes do iPhone")
            return
        }
        guard let fmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(rate), channels: 1, interleaved: false),
              let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(samples.count)) else { return }
        buf.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { src in
            buf.int16ChannelData![0].update(from: src.baseAddress!, count: samples.count)
        }
        let req = SFSpeechAudioBufferRecognitionRequest()
        req.shouldReportPartialResults = false
        var ctx = ["casa", "trabalho", "playlist", "Spotify", "vamos para", "tocar playlist"]
        ctx += places?.favorites.map { $0.name } ?? []
        ctx += playlists?.items.map { $0.name } ?? []
        req.contextualStrings = ctx
        req.append(buf)
        req.endAudio()
        task?.cancel()
        let box = FinishFlag()
        task = recognizer.recognitionTask(with: req) { [weak self] result, error in
            let text = result?.isFinal == true ? result?.bestTranscription.formattedString : nil
            let failed = error != nil
            Task { @MainActor in
                guard let self = self, !box.done else { return }
                if let text = text {
                    box.done = true
                    self.handle(text)
                } else if failed {
                    box.done = true
                    self.reply(false, "Não consegui entender", speak: false)
                }
            }
        }
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 8_000_000_000)
            guard let self = self, !box.done else { return }
            box.done = true
            self.task?.cancel()
            self.reply(false, "Não consegui entender", speak: false)
        }
    }

    private func handle(_ text: String) {
        lastHeard = text
        let favs = places?.favorites ?? []
        switch VoiceParser.parse(text, favorites: favs.map { $0.name }) {
        case .navigate(let target):
            if let i = VoiceParser.bestMatch(target, in: favs.map { $0.name }) {
                let fav = favs[i]
                reply(true, "Indo para \(fav.name)")
                Task { @MainActor in
                    guard let engine = self.engine else { return }
                    await engine.planRoute(to: fav.mapItem)
                    if engine.route != nil { engine.start(simulate: false) }
                }
            } else {
                reply(false, "Não achei o favorito \(target)")
            }
        case .playlist(let target):
            let items = playlists?.items ?? []
            if let i = VoiceParser.bestMatch(target, in: items.map { $0.name }), let url = URL(string: items[i].uri) {
                reply(true, "Tocando \(items[i].name)")
                if UIApplication.shared.applicationState == .active {
                    UIApplication.shared.open(url)
                } else {
                    reply(false, "Abra o app para tocar a playlist", speak: false)
                }
            } else {
                reply(false, "Não achei a playlist \(target)")
            }
        case .stop:
            if engine?.isActive == true {
                engine?.stop()
                reply(true, "Navegação encerrada")
            } else {
                reply(false, "Não há navegação", speak: false)
            }
        case .none:
            reply(false, "Ouvi: \(text)", speak: false)
        }
    }
}
