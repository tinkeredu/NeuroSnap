# NeuroSnap

NeuroSnap ist eine Sofortbildkamera mit ESP32-S3, OV2640 und Thermodrucker. Das Projekt entstand im Informatikunterricht. [Hintergrund zum Unterrichtsprojekt](https://www.tinkeredu.de/informatik/ki-und-klimaschutz-widerspruch-oder-werkzeug/)

Inspiriert vom [Digital Independence Day](https://di.day/) lässt sich der Übertragungsweg für KI-Aufnahmen auswählen.

| Aufnahmemodus | Ausdruck | Bildversand beim Auslösen |
| --- | --- | --- |
| Sofortbild | Eigener Text oben, Foto, eigener Text unten | Keiner |
| Sofortbild mit KI | Eigener Text oben, Foto, Mistral-Text, eigener Text unten | An Mistral |
| Nur KI-Text | Mistral-Text | An Mistral |

In beiden Sofortbildmodi sind die Texte oben und unten optional. Ist die KI nicht erreichbar, druckt **Sofortbild mit KI** weiterhin das Foto und die eigenen Texte. Klicke in der lokalen Galerie auf **Bilder laden**, um Fotos und verfügbare KI-Texte von der SD-Karte abzurufen.

Unter **Einstellungen → Druckrasterung** wählst du die Umwandlung in Schwarz und Weiß. **Bayer** verwendet ein regelmäßiges Punktraster und bleibt die Voreinstellung. **Floyd–Steinberg** erhält feine Grauabstufungen, **Atkinson** betont den Kontrast. **Schwellenwert** eignet sich für Text und Strichzeichnungen. Die Auswahl gilt ab der nächsten Aufnahme. Auf der SD-Karte stehen unter `photo_dither` die Werte `bayer`, `floyd_steinberg`, `atkinson` und `threshold` zur Verfügung.

## Hardware

- ESP32-S3 mit 16 MB Flash und Octal-PSRAM, wie in `platformio.ini` konfiguriert. [Beispiel-Kit](https://de.aliexpress.com/item/1005008589091526.html)
- OV2640-Kamera mit ESP32-S3-EYE-Pinbelegung
- TTL-Thermodrucker mit 384 Druckpunkten pro Zeile und serieller Verbindung mit 9.600 Baud. [Beispiel-Drucker](https://de.aliexpress.com/item/1005007402905573.html)
- MicroSD-Karte mit FAT32, empfohlen sind bis zu 16 GB
- Taster und WS2812B-LED, z. B. an der mitgelieferten Platine

Der Drucker zieht beim Fotodruck deutlich Strom. Im ursprünglichen Aufbau versorgt ihn ein 18650-Akku mit einer Ladeplatine, die 2 A liefern kann.

## Installieren und einrichten

1. Setze eine FAT32-formatierte MicroSD-Karte ein. Für eine Neueinrichtung darf sie leer sein.
2. Baue die Firmware mit [PlatformIO](https://platformio.org/) und flashe den ESP32-S3 per USB.

   ```sh
   pio run -e esp32-s3-dev -t upload
   ```

3. Beim ersten Start öffnet die Kamera nach rund zwölf Sekunden den Hotspot **NeuroSnap_Setup** (WLAN-Passwort `neurosnap`). Verbinde dich damit, öffne bei Bedarf `http://192.168.4.1` und wähle unter **Configure WiFi** dein WLAN. Ein Smartphone-Hotspot geht ebenfalls. Die Kamera druckt ihre IP-Adresse, sobald sie im gewählten Netz ist.
4. Öffne die gedruckte IP-Adresse im Browser aus demselben Netz. Unter **Einstellungen** wählst du den Aufnahmemodus und hinterlegst die Drucktexte. Das Admin-Passwort einer leeren SD-Karte lautet `neurosnap`. Du kannst es dort ändern. Die Geräteoberfläche läuft über lokales HTTP und ist für ein vertrauenswürdiges WLAN vorgesehen.

### WLAN wechseln

Halte die Gerätetaste mindestens zehn Sekunden und lass sie dann los oder klicke in den Webeinstellungen auf **WLAN-Auswahl öffnen**. Das Portal ist drei Minuten verfügbar. Die Kamera kann im Sofortbildmodus schon während der WLAN-Einrichtung aufnehmen und drucken. Mit bereits gespeichertem Sofortbildmodus funktioniert sie bei späteren Starts auch ohne WLAN.

### Mistral und eigener Server

Für die KI-Modi brauchst du Internetzugang und einen [Mistral-API-Schlüssel](https://console.mistral.ai/). Hinterlege ihn unter **Einstellungen → Mistral**. Der Standard-Endpunkt ist `https://api.mistral.ai/v1/chat/completions`, das Standardmodell `mistral-small-latest`. Trage ein Vision-fähiges Modell ein, wenn du den Standard änderst. [Mistral-Dokumentation zu Vision](https://docs.mistral.ai/capabilities/vision/)

Für KI-Aufnahmen werden Foto und Prompt an Mistral gesendet. Die Nutzung dieser Daten zum Modelltraining hängt vom Tarif und den Kontoeinstellungen ab. [Mistral-Datennutzung](https://help.mistral.ai/en/articles/347617-do-you-use-my-user-data-to-train-your-artificial-intelligence-models)

Aktiviere unter **Einstellungen → Mistral** die Option **Bei KI-Fehler Ersatztext drucken** und gib deinen Text ein. Das gilt für **Nur KI-Text** und **Sofortbild mit KI**. Ein leeres Feld blendet den Fehlertext auf dem Beleg aus. Technische Details stehen weiterhin auf der seriellen Konsole und bei aktiviertem SD-Logging im Systemprotokoll. Auf der SD-Karte kannst du die Option mit `"replace_ai_error": true` und den Text mit `"ai_error_text"` in `settings.json` setzen.

**Direkt an Mistral** überträgt das JPEG als Base64 im Chat-Request. Der ESP32 erzeugt und sendet die Base64-Daten in Blöcken von bis zu 2 KiB. Alternativ kannst du **Über eigenen Dropoff-Server** wählen. Dafür muss `Server/dropoff.php` auf einem HTTPS-Webserver erreichbar sein und eine öffentlich abrufbare Bild-URL liefern. Hostname und Pfad sind in den Einstellungen konfigurierbar. Die Kamera wechselt den Uploadweg nur durch deine Auswahl. Beide Wege prüfen das Serverzertifikat und benötigen eine per NTP synchronisierte Uhr. Wenn beim Senden des Mistral-Requests ein Socket-Timeout oder TCP-Reset auftritt und der Body eindeutig unvollständig ist, versucht die Kamera den Request einmal über eine neue Verbindung. Bei einem Schreibabbruch zeigt das Systemprotokoll den Bytefortschritt, den Schreibcode, den Socketfehlercode und die Dauer des fehlgeschlagenen Schreibvorgangs. Nach vollständigem Senden liest die Kamera den HTTP-Status des Servers.

Für den separat ausgelösten **Galerie-Upload** installiere `Server/upload.php` auf demselben Server und trage seinen Pfad ein. Der PHP-Prozess braucht Schreibrechte im jeweiligen Uploadverzeichnis. Die Kamera überträgt nur Dateien mit erfolgreicher KI-Antwort und löscht ein lokales Paar aus Bild und Text nach Bestätigung durch den Galerie-Server. Reine Sofortbilder bleiben lokal. `Server/gallery.php` zeigt die hochgeladenen Aufnahmen an.

### Einstellungen auf der SD-Karte

Bestehende `config.txt`, `prompt.txt` und `password.txt` werden beim Laden als Altkonfiguration berücksichtigt. Bei einer bestehenden Konfiguration mit API-Schlüssel und eigenem Upload-Host bleibt zunächst Dropoff voreingestellt. Du kannst unter **Bildversand** auf den Direktupload wechseln. Ein vorhandener Modellname bleibt erhalten und lässt sich im Browser ändern.

Beim ersten Speichern im Browser entsteht `settings.json` im Hauptverzeichnis der SD-Karte. Ab dann hat diese Datei Vorrang und kann auch bei ausgeschalteter Kamera direkt auf der SD-Karte bearbeitet werden. Sie enthält den API-Schlüssel und das Admin-Passwort. Als Einstieg für ein Gerät ganz ohne WLAN reicht beispielsweise diese Datei `settings.json`:

```json
{
  "mode": "photo",
  "header": "NeuroSnap",
  "footer": "Ein Gruß von der Fotobox!"
}
```

## Bedienung und Fehlersuche

| Taster | Aktion |
| --- | --- |
| Unter 2 Sekunden | Foto aufnehmen und je nach Aufnahmemodus drucken |
| 5 bis unter 10 Sekunden | In einem KI-Modus Bild-Text-Paare zum eigenen Galerie-Server hochladen |
| Ab 10 Sekunden | WLAN-Portal öffnen |

Die LED leuchtet blau im Bereitschaftszustand, gelb beim Verarbeiten einer Aufnahme, cyan während der WLAN-Einrichtung und lila beim Galerie-Upload. Bei Orange findest du den Netzwerk- oder KI-Fehler auf der seriellen Konsole oder bei aktiviertem SD-Logging im Systemprotokoll. Für die Diagnose protokolliert die Firmware vor der KI-Anfrage die JPEG-Größe, den freien Heap, den größten zusammenhängenden Heap-Block und den freien PSRAM. Rot signalisiert einen Aufnahme- oder Fotodruckfehler.

Wenn der Startbeleg lesbar ist, beim Foto aber Sonderzeichen erscheinen, prüfe unter **Einstellungen → Fotodruck-Protokoll** das Bildkommando deines Druckers. Standard ist **ESC/POS (GS v 0)**. Für Drucker mit **Adafruit (DC2 *)** wähle diese Option und speichere. Auf der SD-Karte entspricht das `"photo_protocol": "esc_pos"` bzw. `"photo_protocol": "adafruit"` in `settings.json`. Teste mit einer Aufnahme im Modus **Sofortbild**. Wenn auch der Startbeleg unlesbar ist, prüfe die serielle Verbindung und die Baudrate am Drucker. Bei Abbrüchen während des Druckens prüfe die Stromversorgung.

Kommt die Kamera direkt nach dem WLAN-Start noch nicht per HTTPS ins Netz, warte auf die Zeitsynchronisierung und löse erneut aus. Bei einem HTTPS-Verbindungsfehler gibt das Systemprotokoll verfügbare Transport- und Socketfehler sowie TLS- und Zertifikatsdetails aus. Die Weboberfläche bleibt während der Bildaufbereitung, der Mistral-Anfrage und des Ausdrucks erreichbar. Während eines manuellen Galerie-Uploads kann sie kurz pausieren.

Falls sich der Startbeleg wiederholt, verbinde die Kamera per USB und öffne `pio device monitor -b 115200`. Die Zeile `NeuroSnap Start, ESP32-Resetgrund` und die zuletzt protokollierte Startphase helfen, den Neustart einzugrenzen. Bei aktiviertem SD-Logging stehen die Startmeldungen auch in `system.log`.

## Lizenz

[Creative Commons Namensnennung – Nicht kommerziell – Weitergabe unter gleichen Bedingungen 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
