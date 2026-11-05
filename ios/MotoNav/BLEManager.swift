import Foundation
import CoreBluetooth

final class BLEManager: NSObject, ObservableObject {
    static let serviceUUID = CBUUID(string: "6E0A0001-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let navCharUUID = CBUUID(string: "6E0A0002-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let mapCharUUID = CBUUID(string: "6E0A0003-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    private static let savedPeripheralKey = "motonav.peripheral"

    @Published private(set) var isConnected = false
    @Published private(set) var statusText = "Procurando a placa..."
    @Published private(set) var uploadProgress: Double?

    var onReady: (() -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var navChar: CBCharacteristic?
    private var mapChar: CBCharacteristic?
    private var lastNavPacket: Data?
    private var bulkQueue: [Data] = []
    private var bulkTotal = 0
    private var bulkDone: (() -> Void)?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var maxPayload: Int {
        guard let p = peripheral else { return 20 }
        return min(244, max(20, p.maximumWriteValueLength(for: .withoutResponse)))
    }

    func send(_ data: Data) {
        lastNavPacket = data
        write(data, to: navChar)
    }

    func sendPosition(_ data: Data) {
        guard bulkQueue.isEmpty else { return }
        write(data, to: navChar)
    }

    private func write(_ data: Data, to c: CBCharacteristic?) {
        guard let p = peripheral, let c = c, p.state == .connected else { return }
        let payload = data.prefix(maxPayload)
        if c.properties.contains(.writeWithoutResponse) && p.canSendWriteWithoutResponse {
            p.writeValue(payload, for: c, type: .withoutResponse)
        } else if c.properties.contains(.write) {
            p.writeValue(payload, for: c, type: .withResponse)
        }
    }

    func sendMap(id: UInt16, blob: Data, completion: (() -> Void)? = nil) {
        guard isConnected, mapChar != nil else { return }
        var packets: [Data] = []
        var begin = Data([0x10])
        append(&begin, id)
        append(&begin, UInt32(blob.count))
        packets.append(begin)
        let chunk = max(1, maxPayload - 7)
        var off = 0
        while off < blob.count {
            let end = min(blob.count, off + chunk)
            var p = Data([0x11])
            append(&p, id)
            append(&p, UInt32(off))
            p.append(blob.subdata(in: off..<end))
            packets.append(p)
            off = end
        }
        var endPkt = Data([0x12])
        append(&endPkt, id)
        packets.append(endPkt)
        bulkQueue = packets
        bulkTotal = packets.count
        bulkDone = completion
        uploadProgress = 0
        pump()
    }

    private func pump() {
        guard let p = peripheral, let c = mapChar, p.state == .connected else { return }
        while !bulkQueue.isEmpty && p.canSendWriteWithoutResponse {
            p.writeValue(bulkQueue.removeFirst(), for: c, type: .withoutResponse)
        }
        if bulkTotal > 0 {
            uploadProgress = Double(bulkTotal - bulkQueue.count) / Double(bulkTotal)
        }
        if bulkQueue.isEmpty && bulkTotal > 0 {
            bulkTotal = 0
            uploadProgress = nil
            let done = bulkDone
            bulkDone = nil
            done?()
        }
    }

    private func append<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
        withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) }
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        if let idString = UserDefaults.standard.string(forKey: Self.savedPeripheralKey),
           let id = UUID(uuidString: idString),
           let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(known)
            return
        }
        let connected = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID])
        if let p = connected.first {
            connect(p)
            return
        }
        statusText = "Procurando a placa..."
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        statusText = "Conectando..."
        central.connect(p, options: nil)
    }

    private func resetLink() {
        isConnected = false
        navChar = nil
        mapChar = nil
        bulkQueue = []
        bulkTotal = 0
        bulkDone = nil
        uploadProgress = nil
    }

    func forgetDevice() {
        UserDefaults.standard.removeObject(forKey: Self.savedPeripheralKey)
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        resetLink()
        startScan()
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            statusText = "Permita o Bluetooth nos Ajustes"
        case .poweredOff:
            statusText = "Bluetooth desligado"
            resetLink()
        default:
            statusText = "Bluetooth indisponível"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedPeripheralKey)
        statusText = "Conectando..."
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        resetLink()
        statusText = "Falha ao conectar"
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.startScan() }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetLink()
        statusText = "Placa desconectada"
        central.connect(peripheral, options: nil)
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            statusText = "Placa sem o serviço MotoNav"
            return
        }
        peripheral.discoverCharacteristics([Self.navCharUUID, Self.mapCharUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let c = service.characteristics?.first(where: { $0.uuid == Self.navCharUUID }) else { return }
        navChar = c
        mapChar = service.characteristics?.first(where: { $0.uuid == Self.mapCharUUID })
        isConnected = true
        statusText = mapChar == nil ? "Placa conectada (atualize o firmware)" : "Placa conectada"
        if let last = lastNavPacket { send(last) }
        onReady?()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pump()
    }
}
