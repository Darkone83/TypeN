/* TypeN Web Flasher - direct ESP32 browser installer (esptool-js v0.7.0)
 * Hardware safe: never writes unless the chip family matches the selected build.
 * Copyright (c) 2026 Darkone Customs. Firmware owned by its respective authors.
 */
(() => {
  'use strict';
  const ESPLIB_URL = 'https://unpkg.com/esptool-js@0.7.0/bundle.js';
  const boards = Array.from(document.querySelectorAll('[data-target]'));
  const flashers = Array.from(document.querySelectorAll('[data-installer]'));
  const prompt = document.getElementById('select-prompt');
  const area = document.getElementById('button-area');
  const compat = document.getElementById('compat-notice');
  const followup = document.getElementById('c6-followup');
  const preflight = document.getElementById('firmware-preflight');
  const retry = document.getElementById('firmware-retry');
  const status = document.getElementById('flash-status');
  const progressPanel = document.getElementById('flash-progress-panel');
  const progressBar = document.getElementById('flash-progress');
  const progressLabel = document.getElementById('flash-progress-text');
  const diagnosticPanel = document.getElementById('diagnostic-panel');
  const diagnosticLog = document.getElementById('diagnostic-log');
  const saveLog = document.getElementById('save-diagnostic');
  const retryInstall = document.getElementById('retry-install');
  const readout = {
    title: document.getElementById('selected-name'),
    subtitle: document.getElementById('selected-subtitle'),
    instructions: document.getElementById('connect-instructions'),
    chip: document.getElementById('detail-chip'),
    version: document.getElementById('detail-version'),
    date: document.getElementById('detail-date'),
    size: document.getElementById('detail-size'),
    imageSize: document.getElementById('detail-image-size'),
  };
  const descriptions = {
    'p4-pico': {
      subtitle: 'P4 display firmware · external C6 / UART transport',
      instructions: 'Connect the P4 Pico Type-C programming port to your computer. The external C6 Zero will need its own firmware afterwards.',
    },
    'p4-pico-wifi': {
      subtitle: 'P4 display firmware · onboard C6 / SDIO transport',
      instructions: 'Connect the P4 Pico WiFi Type-C programming port to your computer. Do not use the external C6 slave image for its onboard radio.',
    },
    'c6-external': {
      subtitle: 'Wi-Fi coprocessor firmware · external C6 Zero only',
      instructions: 'Connect the separate ESP32-C6 Zero directly by its USB port. Do not select the P4 port or the onboard C6.',
    },
  };
  let catalog = null;
  let current = null;
  let verificationTicket = 0;
  let verified = null;
  let isInstalling = false;
  let lastLines = [];
  let stage = 'idle';

  function log(line) {
    const stamp = new Date().toLocaleTimeString();
    const message = String(line ?? '').replace(/\r/g, '');
    for (const part of message.split('\n')) {
      if (!part) continue;
      lastLines.push(`[${stamp}] ${part}`);
    }
    if (lastLines.length > 600) lastLines = lastLines.slice(-600);
    diagnosticLog.textContent = lastLines.join('\n');
    diagnosticLog.scrollTop = diagnosticLog.scrollHeight;
    saveLog.disabled = !lastLines.length;
  }

  function showCompatibility() {
    if (!window.isSecureContext) {
      compat.hidden = false;
      compat.textContent = 'A secure HTTPS page is required for USB access. Publish the website using GitHub Pages.';
      return false;
    }
    if (!navigator.serial?.requestPort) {
      compat.hidden = false;
      compat.textContent = 'This browser does not support USB Web Serial. Open the website in desktop Chrome or Edge.';
      return false;
    }
    compat.hidden = true;
    return true;
  }

  function formatBytes(value) {
    return (value / 1024 / 1024).toFixed(2) + ' MB';
  }

  function setInstallEnabled(key, enabled) {
    const wrapper = flashers.find((element) => element.dataset.installer === key);
    const button = wrapper?.querySelector('[data-flash]');
    if (button) button.disabled = !enabled;
  }

  function setAllInstallDisabled() {
    flashers.forEach((wrapper) => setInstallEnabled(wrapper.dataset.installer, false));
  }

  function updateStatus(message, kind = '') {
    status.hidden = false;
    status.textContent = message;
    status.className = 'flash-status' + (kind ? ' flash-status-' + kind : '');
    log(`${stage}: ${message}`);
  }

  function reportPreflight(message, isError = false) {
    preflight.hidden = false;
    preflight.textContent = message;
    preflight.classList.toggle('error-text', isError);
    retry.hidden = !isError;
  }

  // The verified data is reused for flashing; there is no second unchecked download.
  async function verifyFiles(key) {
    const ticket = ++verificationTicket;
    const entry = catalog?.[key];
    if (!entry || isInstalling) return;
    verified = null;
    setInstallEnabled(key, false);
    retry.hidden = true;
    reportPreflight('Verifying firmware from GitHub Pages…');
    try {
      if (!showCompatibility()) throw new Error('Open the page in desktop Chrome/Edge over HTTPS.');
      const manifestUrl = new URL(entry.manifest, location.href);
      const response = await fetch(manifestUrl, {cache: 'no-store'});
      if (!response.ok) throw new Error(`Installer manifest: HTTP ${response.status}`);
      const manifest = await response.json();
      const build = manifest.builds?.find((item) => item.chipFamily === entry.chip);
      if (!build || manifest.version !== entry.version) {
        throw new Error('Manifest is missing the matching chip or release version.');
      }
      if (!Array.isArray(entry.files) || entry.files.length !== 4 ||
          !Array.isArray(build.parts) || build.parts.length !== entry.files.length) {
        throw new Error('Firmware package manifest is incomplete.');
      }
      const images = [];
      for (const component of entry.files) {
        if (ticket !== verificationTicket) return;
        if (!build.parts.some((part) => part.path === component.path && part.offset === component.offset)) {
          throw new Error(`Incorrect firmware offset: ${component.path}.`);
        }
        const fileUrl = new URL(component.path, manifestUrl);
        if (fileUrl.origin !== location.origin) throw new Error('Firmware must be hosted with this website.');
        const fetched = await fetch(fileUrl, {cache: 'no-store'});
        if (!fetched.ok) throw new Error(`${component.path}: HTTP ${fetched.status}`);
        const bytes = new Uint8Array(await fetched.arrayBuffer());
        if (bytes.byteLength !== component.size) throw new Error(`${component.path}: file size mismatch.`);
        const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
        const hash = Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
        if (hash !== component.sha256) throw new Error(`${component.path}: SHA-256 mismatch.`);
        images.push({ data: bytes, address: component.offset, name: component.path });
      }
      if (ticket !== verificationTicket) return;
      verified = {key, images, entry};
      reportPreflight('Firmware ready · all 4 images and SHA-256 hashes verified.');
      setInstallEnabled(key, true);
      stage = 'ready';
    } catch (error) {
      if (ticket !== verificationTicket) return;
      reportPreflight(`Firmware check failed: ${error.message || String(error)}`, true);
      setInstallEnabled(key, false);
    }
  }

  function choose(key, scroll = false) {
    if (isInstalling || !catalog || !catalog[key] || !descriptions[key]) return;
    ++verificationTicket;
    verified = null;
    current = key;
    const entry = catalog[key];
    boards.forEach((button) => {
      const selected = button.dataset.target === key;
      button.classList.toggle('selected', selected);
      button.setAttribute('aria-pressed', String(selected));
    });
    flashers.forEach((element) => { element.hidden = element.dataset.installer !== key; });
    readout.title.textContent = entry.title;
    readout.subtitle.textContent = descriptions[key].subtitle;
    readout.instructions.textContent = descriptions[key].instructions;
    readout.chip.textContent = entry.chip;
    readout.version.textContent = entry.version;
    readout.date.textContent = entry.build_date;
    readout.size.textContent = entry.flash_size;
    readout.imageSize.textContent = formatBytes(entry.image_size);
    followup.hidden = key !== 'p4-pico';
    prompt.hidden = true;
    area.hidden = false;
    progressPanel.hidden = true;
    status.hidden = true;
    retryInstall.hidden = true;
    lastLines = [];
    diagnosticLog.textContent = '';
    diagnosticPanel.open = false;
    showCompatibility();
    verifyFiles(key);
    if (scroll) document.getElementById('install-section').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

  function errorText(e) {
    if (typeof e?.message === 'string') return e.message;
    return String(e);
  }

  function classifyError(error, stageName) {
    const msg = errorText(error);
    if (error?.name === 'NotFoundError' || error?.name === 'NotAllowedError' || /user cancelled|no port selected|cancelled/i.test(msg)) {
      return 'USB port selection was cancelled. Nothing was written.';
    }
    if (/already open|port is in use|failed to open|access denied|device is busy|port is locked/i.test(msg)) {
      return 'The USB serial port is busy. Close other serial tools (including the desktop flasher), reconnect the USB cable, and retry.';
    }
    if (stageName === 'connect') {
      return `Could not automatically initialize the ESP chip. Confirm that the Type-C programming USB port is selected, then retry. Details: ${msg}`;
    }
    if (stageName === 'library') {
      return `Could not load the browser flash engine. Check your connection to unpkg.com. Details: ${msg}`;
    }
    if (stageName === 'flash') {
      return `Firmware transfer failed. Do not disconnect or restart during an active flash. Details: ${msg}`;
    }
    return `Installer error: ${msg}`;
  }

  async function chooseReplacementPort(port) {
    const info = port.getInfo?.() || {};
    if (info.usbVendorId == null) return port;
    const available = await navigator.serial.getPorts();
    const matches = available.filter(candidate => {
      const other = candidate.getInfo?.() || {};
      return other.usbVendorId === info.usbVendorId && other.usbProductId === info.usbProductId;
    });
    if (matches.length === 1) return matches[0];
    return port;
  }

  // Use Espressif's native reset negotiation, not ESP Web Tools' generic modal.
  // A second automatic attempt handles slow Windows USB port re-enumeration.
  async function connectAutomatically(ESPLoader, Transport, selectedPort, entry) {
    let lastError = null;
    for (const [attemptIndex, mode] of ['default_reset', 'default_reset'].entries()) {
      let transport = null;
      try {
        const port = attemptIndex ? await chooseReplacementPort(selectedPort) : selectedPort;
        log(`Automatic connection attempt ${attemptIndex + 1}/2, mode=${mode}`);
        transport = new Transport(port, true);
        const loader = new ESPLoader({
          transport,
          baudrate: 115200,
          debugLogging: false,
          terminal: {
            clean() {},
            write(data) { log(String(data)); },
            writeLine(data) { log(String(data)); },
          },
        });
        const chip = await loader.main(mode);
        log(`Chip detected: ${chip}`);
        const canonicalChip = String(chip || '').toUpperCase().replace(/[_\s]+/g, '-');
        if (!canonicalChip.includes(entry.chip.toUpperCase())) {
          const mismatch = new Error(`Wrong device selected. Detected ${chip}, expected ${entry.chip}. No firmware was written.`);
          mismatch.chipMismatch = true;
          throw mismatch;
        }
        return {transport, loader};
      } catch (error) {
        lastError = error;
        log(`Connection attempt ${attemptIndex + 1} failed: ${errorText(error)}`);
        if (transport) { try { await transport.disconnect(); } catch (e) { log(`Cleanup: ${errorText(e)}`); } }
        if (error.chipMismatch) throw error;
        if (attemptIndex === 0) { updateStatus('First USB connection attempt failed; retrying automatic reset…'); await delay(900); }
      }
    }
    throw lastError || new Error('No response from the ROM bootloader');
  }

  async function install(key) {
    if (isInstalling || key !== current || !verified || verified.key !== key || !showCompatibility()) return;
    if (!catalog?.[key]) return;
    isInstalling = true;
    setAllInstallDisabled();
    boards.forEach(button => { button.disabled = true; });
    retry.hidden = true;
    retryInstall.hidden = true;
    diagnosticPanel.open = false;
    lastLines = [];
    diagnosticLog.textContent = '';
    const firmware = verified;
    const entry = firmware.entry;
    let transport = null;
    let loader = null;
    stage = 'library';
    try {
      log(`TypeN Web Flasher · direct esptool-js 0.7.0 · ${entry.title} · ${entry.version}`);
      updateStatus('Loading the browser flash engine…');
      const { ESPLoader, Transport } = await import(ESPLIB_URL);
      if (!ESPLoader || !Transport) throw new Error('Flash engine did not export ESPLoader and Transport.');
      stage = 'connect';
      updateStatus('Choose your TypeN USB programming port. Automatic reset is enabled.');
      const port = await navigator.serial.requestPort();
      const ids = port.getInfo?.() || {};
      log(`Selected USB port: VID=${ids.usbVendorId == null ? 'unavailable' : '0x' + ids.usbVendorId.toString(16)}, PID=${ids.usbProductId == null ? 'unavailable' : '0x' + ids.usbProductId.toString(16)}`);
      ({ transport, loader } = await connectAutomatically(ESPLoader, Transport, port, entry));
      stage = 'flash';
      updateStatus(`Connected to ${entry.chip}. Installing ${entry.version}…`);
      progressPanel.hidden = false;
      progressBar.value = 0;
      progressLabel.textContent = 'Preparing flash…';
      const sizes = firmware.images.map(file => file.data.byteLength);
      const totalSize = sizes.reduce((sum, size) => sum + size, 0);
      await loader.writeFlash({
        fileArray: firmware.images.map(file => ({ data: file.data, address: file.address })),
        flashMode: entry.flash_mode || 'dio',
        flashFreq: entry.flash_freq || '80m',
        flashSize: entry.flash_size,
        eraseAll: false,
        compress: true,
        reportProgress(fileIndex, written, total) {
          if (fileIndex < 0 || fileIndex >= sizes.length) return;
          const prior = sizes.slice(0, fileIndex).reduce((sum, size) => sum + size, 0);
          const pct = Math.min(100, Math.floor(100 * (prior + Math.min(written, sizes[fileIndex])) / totalSize));
          progressBar.value = pct;
          progressLabel.textContent = `${pct}% · ${firmware.images[fileIndex].name} (${Math.floor(100 * written / (total || 1))}%)`;
        },
      });
      progressBar.value = 100;
      progressLabel.textContent = 'All firmware images written.';
      log('All firmware parts were written successfully.');
      // A disconnected/re-enumerated USB Serial/JTAG interface does not mean flashing failed.
      try {
        await loader.after('hard_reset');
        log('Requested automatic reboot.');
      } catch (e) {
        log(`Firmware written; automatic reboot was not confirmed: ${errorText(e)}`);
      }
      stage = 'success';
      updateStatus('Firmware installed successfully. TypeN should restart; if it does not, press RESET once.', 'success');
      diagnosticPanel.open = false;
    } catch (error) {
      log(`${stage} error: ${error?.stack || errorText(error)}`);
      const wasMismatch = Boolean(error?.chipMismatch);
      const userText = wasMismatch ? errorText(error) : classifyError(error, stage);
      updateStatus(userText, 'error');
      diagnosticPanel.open = true;
      retryInstall.hidden = false;
      progressPanel.hidden = stage !== 'flash';
    } finally {
      if (transport) {
        try { await transport.disconnect(); } catch (error) { log(`Disconnect: ${errorText(error)}`); }
      }
      isInstalling = false;
      boards.forEach(button => { button.disabled = false; });
      if (verified?.key === current && showCompatibility()) setInstallEnabled(current, true);
    }
  }

  boards.forEach(button => button.addEventListener('click', () => choose(button.dataset.target)));
  document.getElementById('select-c6').addEventListener('click', () => choose('c6-external', true));
  retry.addEventListener('click', () => { if (current && !isInstalling) verifyFiles(current); });
  retryInstall.addEventListener('click', () => { if (current && !isInstalling) install(current); });
  flashers.forEach(wrapper => wrapper.querySelector('[data-flash]')?.addEventListener('click', () => install(wrapper.dataset.installer)));
  saveLog.addEventListener('click', () => {
    const content = lastLines.join('\n') + '\n';
    const blob = new Blob([content], {type:'text/plain'});
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `TypeN-WebFlasher-${current || 'diagnostics'}.txt`;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  });

  async function initialize() {
    try {
      const response = await fetch('./firmware/catalog.json', {cache:'no-store'});
      if (!response.ok) throw new Error(`Firmware catalog HTTP ${response.status}`);
      const data = await response.json();
      if (data.schema_version !== 2 || !Array.isArray(data.firmwares)) throw new Error('Firmware catalog is invalid.');
      catalog = Object.fromEntries(data.firmwares.map(entry => [entry.key, entry]));
      if (['p4-pico', 'p4-pico-wifi', 'c6-external'].some(key => !catalog[key])) {
        throw new Error('One or more firmware variants are missing.');
      }
      showCompatibility();
    } catch (error) {
      catalog = null;
      prompt.textContent = 'Firmware is not available. Reload the GitHub Pages site or contact the maintainer.';
      prompt.classList.add('error-text');
      compat.hidden = false;
      compat.textContent = `Setup error: ${errorText(error)}`;
      boards.forEach(button => { button.disabled = true; });
    }
  }

  showCompatibility();
  initialize();
})();
