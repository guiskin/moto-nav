import Foundation
import MapKit

final class SearchModel: NSObject, ObservableObject, MKLocalSearchCompleterDelegate {
    @Published var query = "" {
        didSet { completer.queryFragment = query }
    }
    @Published private(set) var results: [MKLocalSearchCompletion] = []

    private let completer = MKLocalSearchCompleter()

    override init() {
        super.init()
        completer.delegate = self
        completer.resultTypes = [.address, .pointOfInterest]
    }

    func setRegion(around coordinate: CLLocationCoordinate2D, meters: Double = 50_000) {
        completer.region = MKCoordinateRegion(center: coordinate, latitudinalMeters: meters, longitudinalMeters: meters)
    }

    func setQueryQuietly(_ text: String) {
        completer.delegate = nil
        query = text
        completer.delegate = self
    }

    func resolve(_ completion: MKLocalSearchCompletion) async -> MKMapItem? {
        let req = MKLocalSearch.Request(completion: completion)
        let resp = try? await MKLocalSearch(request: req).start()
        return resp?.mapItems.first
    }

    func clear() {
        query = ""
        results = []
    }

    func completerDidUpdateResults(_ completer: MKLocalSearchCompleter) {
        results = completer.results
    }

    func completer(_ completer: MKLocalSearchCompleter, didFailWithError error: Error) {
        results = []
    }
}
