import Foundation
import AVFoundation

final class VoiceGuide: NSObject, AVSpeechSynthesizerDelegate {
    private let synth = AVSpeechSynthesizer()
    private var spoken = Set<String>()
    private let voice = AVSpeechSynthesisVoice(language: "pt-BR")

    var enabled: Bool {
        get { UserDefaults.standard.object(forKey: "motonav.voice") as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: "motonav.voice") }
    }

    override init() {
        super.init()
        synth.delegate = self
    }

    func reset() {
        spoken.removeAll()
        synth.stopSpeaking(at: .immediate)
    }

    func say(_ text: String, once key: String? = nil) {
        guard enabled, !text.isEmpty else { return }
        if let k = key {
            guard !spoken.contains(k) else { return }
            spoken.insert(k)
        }
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .voicePrompt, options: [.duckOthers, .interruptSpokenAudioAndMixWithOthers])
        try? session.setActive(true)
        let u = AVSpeechUtterance(string: text)
        u.voice = voice
        u.rate = AVSpeechUtteranceDefaultSpeechRate * 1.05
        synth.speak(u)
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        guard !synthesizer.isSpeaking else { return }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    static func spokenDistance(_ m: Double) -> String {
        if m >= 1000 {
            let km = (m / 100).rounded() / 10
            let s = km == km.rounded() ? String(Int(km)) : String(format: "%.1f", km).replacingOccurrences(of: ".", with: ",")
            return "\(s) quilômetro\(km == 1 ? "" : "s")"
        }
        let r = m < 100 ? (m / 10).rounded() * 10 : (m / 50).rounded() * 50
        return "\(Int(r)) metros"
    }
}
