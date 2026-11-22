#pragma once

#include <Arduino.h>

const char CAMERA_WEB_PAGE[] PROGMEM = R"html(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NeuroSnap</title>
  <style>
    :root { color-scheme:dark; font:16px/1.5 system-ui,sans-serif; color:#eeeaf4; background:#151318; }
    * { box-sizing:border-box; }
    body { margin:0; padding:clamp(14px,4vw,32px); }
    main { max-width:840px; margin:auto; }
    header, .toolbar { display:flex; gap:12px; justify-content:space-between; align-items:center; flex-wrap:wrap; }
    h1,h2,h3 { margin:0 0 12px; }
    header h1 { margin:0; }
    section,details { background:#211e26; border:1px solid #3a3442; border-radius:12px; padding:clamp(14px,3vw,22px); margin:18px 0; }
    summary { cursor:pointer; }
    button,.download { border:1px solid transparent; border-radius:7px; padding:8px 14px; background:#d2bbfb; font:inherit; font-weight:650; color:#221639; cursor:pointer; text-decoration:none; }
    button.secondary,.download { border-color:#5e536d; background:transparent; color:inherit; }
    button:disabled { opacity:.55; cursor:wait; }
    button:focus-visible,a:focus-visible,summary:focus-visible { outline:2px solid #dccbff; outline-offset:3px; }
    label { display:block; margin:10px 0; }
    input:not([type=checkbox]),select,textarea { display:block; width:100%; padding:8px; background:#151318; color:inherit; border:1px solid #63586e; border-radius:6px; font:inherit; }
    textarea { resize:vertical; }
    fieldset { border:0; border-top:1px solid #51475d; margin:16px 0; padding:0; min-width:0; }
    legend { font-weight:700; padding-right:8px; }
    .muted { color:#c8bed2; }
    .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(min(100%,240px),1fr)); gap:14px; }
    article { border:1px solid #4a4253; border-radius:8px; overflow:hidden; }
    article img { display:block; width:100%; height:auto; }
    article div { padding:12px; }
    pre { font:inherit; white-space:pre-wrap; overflow-wrap:anywhere; }
    [hidden] { display:none !important; }
  </style>
</head>
<body>
<main>
  <header>
    <h1>NeuroSnap</h1>
    <button id="admin" type="button">Einstellungen</button>
  </header>
  <p id="status" role="status" hidden></p>

  <section id="settings" hidden>
    <h2>Einstellungen</h2>
    <form id="settingsForm">
      <label>Aufnahmemodus
        <select name="mode">
          <option value="photo">Sofortbild · Foto und eigene Texte</option>
          <option value="photo_ai">Sofortbild mit KI · Foto, KI-Text und eigene Texte</option>
          <option value="ai_text">Nur KI-Text</option>
        </select>
      </label>
      <div id="printTexts">
        <label>Text über dem Foto <textarea name="header" rows="2" maxlength="512"></textarea></label>
        <label>Text unter dem Foto <textarea name="footer" rows="2" maxlength="512"></textarea></label>
        <label>Druckrasterung
          <select name="photo_dither">
            <option value="bayer">Bayer · regelmäßiges Punktraster</option>
            <option value="floyd_steinberg">Floyd–Steinberg · feine Grauabstufungen</option>
            <option value="atkinson">Atkinson · kontrastreiche Fotos</option>
            <option value="threshold">Schwellenwert · Text und Strichzeichnungen</option>
          </select>
        </label>
        <label>Fotodruck-Protokoll
          <select name="photo_protocol">
            <option value="esc_pos">ESC/POS (GS v 0)</option>
            <option value="adafruit">Adafruit (DC2 *)</option>
          </select>
        </label>
      </div>
      <fieldset id="visionFields">
        <legend>Mistral</legend>
        <label>KI-Prompt <textarea name="prompt" rows="3" maxlength="3000"></textarea></label>
        <label><input name="replace_ai_error" type="checkbox"> Bei KI-Fehler Ersatztext drucken</label>
        <label id="aiErrorTextRow" hidden>Ersatztext bei KI-Fehler
          <textarea name="ai_error_text" rows="2" maxlength="512" placeholder="Leer lassen, um keinen Fehlertext zu drucken"></textarea>
        </label>
        <label>Bildversand
          <select name="transfer">
            <option value="direct">Direkt an Mistral</option>
            <option value="dropoff">Über eigenen Dropoff-Server</option>
          </select>
        </label>
        <label>API-Schlüssel
          <input name="api_key" type="password" maxlength="256" autocomplete="off">
        </label>
        <label id="clearKeyRow"><input type="checkbox" id="clearKey"> Gespeicherten API-Schlüssel entfernen</label>
        <label>API-Endpunkt <input name="api_url" type="url" maxlength="256" required></label>
        <label>Vision-Modell <input name="api_model" maxlength="96" required></label>
      </fieldset>
      <fieldset>
        <legend>Eigener Server</legend>
        <label>Hostname <input name="upload_host" maxlength="253" required></label>
        <label>Pfad für Galerie-Upload <input name="upload_path" maxlength="256" required></label>
        <label>Pfad für Dropoff <input name="dropoff_path" maxlength="256" required></label>
      </fieldset>
      <fieldset>
        <legend>Gerät</legend>
        <label><input name="logging_enabled" type="checkbox"> Protokoll auf SD-Karte speichern</label>
        <label><input name="guided_mode" type="checkbox"> Beim Start Bedienhinweise drucken</label>
        <label>Neues Admin-Passwort
          <input name="admin_password" type="password" maxlength="63" autocomplete="new-password" placeholder="Leer lassen, um es zu behalten">
        </label>
      </fieldset>
      <button id="save" type="submit">Speichern</button>
      <span id="sdNotice" class="muted" hidden>Zum Speichern wird eine SD-Karte benötigt.</span>
    </form>
    <fieldset>
      <legend>WLAN</legend>
      <button type="button" class="secondary" id="wifi">WLAN-Auswahl öffnen</button>
      <p id="wifiInfo" class="muted" hidden></p>
    </fieldset>
    <fieldset>
      <legend>Systemprotokoll</legend>
      <textarea id="log" rows="6" readonly aria-label="Systemprotokoll"></textarea>
      <p><button type="button" class="secondary" id="readLog">Aktualisieren</button>
        <button type="button" class="secondary" id="deleteLog">Log löschen</button></p>
    </fieldset>
    <form id="firmware">
      <fieldset>
        <legend>Firmware</legend>
        <label>Firmware-Datei <input name="firmware" type="file" accept=".bin" required></label>
        <button type="submit" class="secondary">Firmware aktualisieren</button>
      </fieldset>
    </form>
  </section>

  <section>
    <div class="toolbar">
      <h2>Galerie</h2>
      <button id="loadPhotos" class="secondary" type="button">Bilder laden</button>
    </div>
    <div id="gallery" class="grid" aria-live="polite"></div>
  </section>
  <details>
    <summary>Gerätetaste und LED</summary>
    <ul>
      <li>Kurz drücken: Foto aufnehmen und nach gewähltem Modus drucken.</li>
      <li>5–10 Sekunden: Bilder mit KI-Text zur eigenen Galerie übertragen.</li>
      <li>Ab 10 Sekunden: WLAN-Auswahl öffnen.</li>
      <li>Blau: bereit. Gelb: Aufnahme wird verarbeitet. Cyan: WLAN-Auswahl. Lila: Galerie-Upload. Orange: Netzwerk- oder KI-Fehler. Rot: Kamera- oder Druckfehler.</li>
    </ul>
  </details>
</main>
<script>
  const byId = id => document.getElementById(id);
  const form = byId('settingsForm');
  const fields = form.elements;
  const settingNames = [
    'mode', 'header', 'footer', 'photo_dither', 'photo_protocol', 'prompt', 'replace_ai_error', 'ai_error_text',
    'transfer', 'api_url', 'api_model',
    'upload_host', 'upload_path', 'dropoff_path', 'logging_enabled', 'guided_mode'
  ];
  let auth = '';

  function status(message) {
    byId('status').textContent = message;
    byId('status').hidden = !message;
  }

  function credentials(password) {
    return 'Basic ' + btoa(String.fromCharCode(...new TextEncoder().encode('admin:' + password)));
  }

  async function adminRequest(path, init = {}) {
    if (!auth) {
      const password = prompt('Admin-Passwort eingeben');
      if (!password) throw new Error('Eingabe abgebrochen.');
      auth = credentials(password);
    }
    const headers = new Headers(init.headers);
    headers.set('Authorization', auth);
    if (init.method === 'POST') headers.set('X-NeuroSnap-Request', '1');
    const response = await fetch(path, {...init, headers, cache:'no-store'});
    if (response.status === 401) auth = '';
    if (!response.ok) throw new Error(await response.text() || 'HTTP ' + response.status);
    return response;
  }

  async function run(button, operation) {
    button.disabled = true;
    status('');
    try { await operation(); } catch (error) { status(error.message); }
    finally { button.disabled = false; }
  }

  async function loadGallery() {
    const gallery = byId('gallery');
    const response = await fetch('/list', {cache:'no-store'});
    if (!response.ok) throw new Error('Galerie konnte nicht geladen werden.');
    const files = (await response.json()).filter(file => /^\/bild_\d+\.jpg$/.test(file.name));
    files.sort((a, b) => b.name.localeCompare(a.name, 'de', {numeric:true}));
    gallery.replaceChildren();
    if (!files.length) gallery.textContent = 'Noch keine Fotos auf der SD-Karte.';
    for (const file of files) {
      const article = document.createElement('article');
      const photo = document.createElement('img');
      photo.src = '/sd' + file.name;
      photo.alt = 'Aufnahme ' + file.name.slice(6, -4);
      photo.loading = 'lazy';
      const body = document.createElement('div');
      const caption = document.createElement('pre');
      caption.hidden = true;
      const actions = document.createElement('div');
      actions.className = 'toolbar';
      const download = document.createElement('a');
      download.className = 'download';
      download.href = photo.src;
      download.download = '';
      download.textContent = 'Download';
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'secondary';
      remove.textContent = 'Löschen';
      remove.onclick = () => {
        if (!confirm('Dieses Foto löschen?')) return;
        run(remove, async () => {
          await adminRequest('/delete', {method:'POST', body:file.name.slice(1)});
          article.remove();
          if (!gallery.childElementCount) gallery.textContent = 'Noch keine Fotos auf der SD-Karte.';
        });
      };
      actions.append(download, remove);
      body.append(caption, actions);
      article.append(photo, body);
      gallery.append(article);
      fetch('/sd' + file.name.replace(/\.jpg$/, '.txt'), {cache:'no-store'})
        .then(reply => reply.ok ? reply.text() : '')
        .then(text => {
          caption.textContent = text;
          caption.hidden = !text;
        }).catch(() => {});
    }
  }

  function setVisibility() {
    byId('printTexts').hidden = fields.mode.value === 'ai_text';
    byId('visionFields').hidden = fields.mode.value === 'photo';
    byId('aiErrorTextRow').hidden = !fields.replace_ai_error.checked;
  }

  function showKeyStatus(configured) {
    fields.api_key.value = '';
    fields.api_key.placeholder = configured ? 'Gespeichert · leer lassen, um ihn zu behalten' : '';
    byId('clearKey').checked = false;
    byId('clearKeyRow').hidden = !configured;
  }

  byId('admin').onclick = () => run(byId('admin'), async () => {
    const settings = await (await adminRequest('/settings')).json();
    for (const name of settingNames) {
      const field = fields[name];
      if (field.type === 'checkbox') field.checked = settings[name];
      else field.value = settings[name];
    }
    showKeyStatus(settings.api_key_configured);
    fields.admin_password.value = '';
    form.dataset.sdReady = settings.sd_ready;
    byId('save').disabled = !settings.sd_ready;
    byId('sdNotice').hidden = settings.sd_ready;
    setVisibility();
    byId('settings').hidden = false;
    byId('settings').scrollIntoView({block:'start'});
  });
  fields.mode.onchange = setVisibility;
  fields.replace_ai_error.onchange = setVisibility;
  form.onsubmit = event => {
    event.preventDefault();
    if (form.dataset.sdReady !== 'true') return;
    run(byId('save'), async () => {
      const settings = {};
      for (const name of settingNames) {
        const field = fields[name];
        settings[name] = field.type === 'checkbox' ? field.checked : field.value;
      }
      if (byId('clearKey').checked) settings.api_key = '';
      else if (fields.api_key.value.trim()) settings.api_key = fields.api_key.value.trim();
      if (fields.admin_password.value) settings.admin_password = fields.admin_password.value;
      const response = await adminRequest('/settings', {
        method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(settings)
      });
      if (settings.admin_password) auth = credentials(settings.admin_password);
      if ('api_key' in settings) showKeyStatus(!!settings.api_key);
      fields.admin_password.value = '';
      status(await response.text());
    });
  };
  byId('wifi').onclick = () => run(byId('wifi'), async () => {
    const response = await adminRequest('/wifi', {method:'POST'});
    byId('wifiInfo').textContent = await response.text();
    byId('wifiInfo').hidden = false;
  });
  async function readLog() {
    byId('log').value = await (await adminRequest('/getLog')).text();
  }
  byId('readLog').onclick = () => run(byId('readLog'), readLog);
  byId('deleteLog').onclick = () => {
    if (!confirm('Systemprotokoll löschen?')) return;
    run(byId('deleteLog'), async () => {
      await adminRequest('/deleteLog', {method:'POST'});
      await readLog();
    });
  };
  byId('firmware').onsubmit = event => {
    event.preventDefault();
    run(event.submitter, async () => {
      const response = await adminRequest('/update', {method:'POST', body:new FormData(event.target)});
      status(await response.text());
    });
  };
  byId('loadPhotos').onclick = () => run(byId('loadPhotos'), loadGallery);
</script>
</body>
</html>
)html";
