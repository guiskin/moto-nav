# MotoNav — documento de continuidade (Placa Dev)

Repositório: https://github.com/guiskin/moto-nav (branch `main`). Todo o código está no repositório; nada ficou só na conversa.

## 1. Objetivo

GPS de moto próprio, no estilo Beeline Moto II, em uma placa **Waveshare ESP32-S3-Touch-AMOLED-1.75-G** (tela redonda 466×466, GPS LC76G, bateria AXP2101, 2 microfones ES7210, saída de áudio ES8311).
O app de iPhone calcula a rota com o Apple Maps e envia tudo por BLE; a placa mostra seta/distância/rua ou um mapa de ruas girando com a direção.

Usuário: Guilherme, consultor SAP/ABAP, iPhone + Windows, sem Mac, GitHub `guiskin`, fala português do Brasil.

## 2. O que já está pronto

**Placa (firmware Arduino, `firmware/MotoNav/MotoNav.ino`, ~2150 linhas)**
- Telas: mapa (ruas OSM + rota), seta + distância + rua, música (iOS via AMS), ociosa. Toque troca de tela; botão BOOT também.
- Modo noturno (mapa escuro), brilho dia/noite, bateria sempre visível, tela apaga em 30 s sem rota (`displayOff/displayOn`).
- Alerta de curva, limite de velocidade, radar (borda vermelha, pisca branco/vermelho, distância), notificações do iPhone (ANCS), música (AMS), toast.
- OTA pelo app (buffer em PSRAM, depois `Update`).
- GPS próprio via I2C (LC76G) com modo "passeio".

**App iPhone (`ios/MotoNav/*.swift`, SwiftUI, XcodeGen)**
- Busca (MKLocalSearchCompleter), favoritos e recentes, prévia da rota, modo "Passeio sem rota", recálculo automático, ETA compartilhável.
- Mapa de ruas via Overpass (rota primeiro, depois blocos de 800 m), limites de velocidade e radares (`speed_camera`).
- Avisos de voz simples (AVSpeechSynthesizer), histórico de viagens, ajustes (brilho, noite, notificações), atualização de firmware pelo app.
- Roda em segundo plano (localização, bluetooth, áudio).

**CI (`.github/workflows/build.yml`)**: a cada push gera `MotoNav.ipa` (macos-15, sem assinatura) e o firmware; publica nos releases `app` e `firmware` (`MotoNav-firmware.bin`, `MotoNav-app.bin`, `version.txt`). Versão do firmware = `github.run_number`.

**Última alteração (commit 8a1756b)**: alerta de radar no padrão Waze: antecedência por velocidade (clamp(velocidade×12 s, 300–600 m)), voz "Radar à frente", borda vermelha fixa até passar. Falta confirmar que o build terminou e instalar.

## 3. Decisões tomadas

- Firmware próprio (não engenharia reversa do original). Placa 1.75-G (com GPS).
- iPhone sem Mac: build no GitHub Actions + instalação com **Sideloadly** (certificado gratuito vale 7 dias; renovar reinstalando).
- Rota pelo Apple Maps; mapa desenhado na placa (não streaming de tiles) com dados OSM via Overpass.
- Voz: avisos curtos e únicos ("Em 50 metros, vire à direita"), sem bip. Radar: "Radar à frente".
- Tela: 30 s sem uso e sem rota → apaga; bateria sempre visível.
- Evitar `driver/i2c.h` legado (conflita com Wire); usar Wire com mutex.
- Toque com `setMirrorXY(true, true)`.
- Voz no futuro: toque duplo no BOOT ou "Hey MotoNav" ativam o microfone da placa; reconhecimento no iPhone (pt-BR); Spotify por playlist cadastrada.

## 4. O que falta

1. Instalar v18+ na placa (cabo USB com dados; BOOT+RESET se o PC não enxergar; ou pelo app em Ajustes → Atualizar a placa) e reinstalar o IPA.
2. **Comando de voz** (não implementado):
   - Duplo clique no BOOT liga o microfone; "Hey MotoNav" exige palavra de ativação treinada (ESP-SR só traz palavras prontas como "Hi ESP"/"Alexa").
   - ES7210 captura → envia áudio comprimido por BLE → app transcreve com `SFSpeechRecognizer` pt-BR → intenções: "vamos pra casa/trabalho" (favoritos), "tocar playlist X".
   - Spotify: app guarda mapa nome→URI de playlist e abre `spotify:playlist:<id>` (Spotify instalado; reprodução automática pode exigir o SDK App Remote com Client ID de desenvolvedor).
   - Feedback por voz pelo alto-falante da placa (ES8311 + NS4150B) ou pelo iPhone.
3. Atualizar o `README.md` (protocolo antigo) com o protocolo abaixo.
4. Opcionais: tema de mapa claro de dia, contador de combustível, atalhos do Siri.

## 5. Protocolo BLE (atual)

Serviço `6e0a0001-7a3b-4c2d-9e1f-0b5a4d3c2b1a`.

| Característica | UUID | Uso |
| --- | --- | --- |
| nav/pos/alert/status/settings | `…0002` | pacotes 0x01–0x05 |
| mapa | `…0003` | 0x10 início, 0x11 bloco, 0x12 fim |
| info | `…0004` | read/notify: `fw=N`, `ota=…` |
| OTA | `…0005` | 0x20 início, 0x21 dados, 0x22 fim |

Pacotes: 0x01 navegação (manobra, ângulo, distâncias, ETA, velocidade, flags, rua), 0x02 posição (x/y em dm, rumo, velocidade, distância percorrida), 0x03 alerta de curva, 0x04 status (limite, distância do radar `0xFFFF` = nenhum, rua), 0x05 ajustes (brilho, noite, notificações), 0xA0 notificação interna iOS.
Blobs de mapa: `MNR2` (rota, sessão u16) e `MNT2` (blocos de 800 m, tx, ty); linhas com deltas i16; tipos 0 rota, 1–4 classes de via.

## 6. Hardware

- LCD QSPI: SDIO0–3 = 4,5,6,7; SCLK 38; CS 12; RESET 39. I2C SDA 15 / SCL 14. Touch INT 11, RESET 40. BOOT = GPIO 0.
- GPS LC76G em I2C 0x50/0x54 (consulta de tamanho e depois leitura, atrasos de 100 ms).
- Áudio: ES8311 0x18 (reprodução), ES7210 0x40 (microfones). I2S MCLK 42, BCLK 9, LRCK 45, DOUT 8, DIN 10. Perfil de voz do BSP: 24 kHz / 16 bits.
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc`, core esp32 3.3.10.
- Bibliotecas: GFX Library for Arduino, SensorLib, XPowersLib (do repositório `waveshareteam/ESP32-S3-Touch-AMOLED-1.75`, pasta `examples/arduino/libraries`), U8g2, TinyGPSPlus.

## 7. Arquivos para baixar

- Repositório inteiro (zip do `main`): https://github.com/guiskin/moto-nav/archive/refs/heads/main.zip
- `MotoNav.ipa`: release `app`. Firmware: release `firmware`.
- `tools/hosttest/` — simulador no Linux (stubs + `preview.cpp`) que gera PNG das telas; precisa do `.ino` convertido em `motonav_src.cpp`.
- Imagens das telas (`placa_dia.png`, `placa_noite.png`, `placa_noite_mapa.png`, `placa_radar_bateria.png`): opcionais, só referência visual.

## 8. Para continuar no Codex

1. Clonar `guiskin/moto-nav`.
2. Ler este arquivo, depois `NavigationEngine.swift`, `BLEManager.swift` e `MotoNav.ino`.
3. Todo push dispara o build; conferir em Actions e em Releases.
4. Começar pelo item 2 de "O que falta" (voz), em passos: duplo clique BOOT → captura do microfone → envio BLE → transcrição no app → intenções.
