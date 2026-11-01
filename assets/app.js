(() => {
  'use strict';
  const boards = Array.from(document.querySelectorAll('[data-target]'));
  const flashers = Array.from(document.querySelectorAll('[data-installer]'));
  const prompt = document.getElementById('select-prompt');
  const area = document.getElementById('button-area');
  const compat = document.getElementById('compat-notice');
  const followup = document.getElementById('c6-followup');
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
      instructions: 'Connect the P4 Pico USB port to your computer. The separate C6 Zero will need its own flash afterwards.',
    },
    'p4-pico-wifi': {
      subtitle: 'P4 display firmware · onboard C6 / SDIO transport',
      instructions: 'Connect the P4 Pico WiFi USB port to your computer. Do not use the external C6 slave image for its onboard radio.',
    },
    'c6-external': {
      subtitle: 'Wi-Fi coprocessor firmware · external C6 Zero only',
      instructions: 'Connect the separate ESP32-C6 Zero directly by its own USB port. Do not select the P4 port or an onboard C6.',
    },
  };

  let catalog = null;
  let current = null;
  let available = false;

  function showCompatibility() {
    if (!window.isSecureContext) {
      compat.hidden = false;
      compat.textContent = 'USB access requires a secure HTTPS site. Publish these files with GitHub Pages before flashing.';
      return false;
    }
    if (!('serial' in navigator)) {
      compat.hidden = false;
      compat.textContent = 'Web Serial is not available here. Open this page in Chrome or Edge on a desktop computer.';
      return false;
    }
    compat.hidden = true;
    return true;
  }

  function formatBytes(value) {
    return (value / 1024 / 1024).toFixed(2) + ' MB';
  }

  function choose(key, scroll = false) {
    if (!catalog || !catalog[key] || !descriptions[key]) return;
    current = key;
    const item = catalog[key];
    boards.forEach((button) => {
      const selected = button.dataset.target === key;
      button.classList.toggle('selected', selected);
      button.setAttribute('aria-pressed', String(selected));
    });
    flashers.forEach((element) => { element.hidden = element.dataset.installer !== key; });
    readout.title.textContent = item.title;
    readout.subtitle.textContent = descriptions[key].subtitle;
    readout.instructions.textContent = descriptions[key].instructions;
    readout.chip.textContent = item.chip;
    readout.version.textContent = item.version;
    readout.date.textContent = item.build_date;
    readout.size.textContent = item.flash_size;
    readout.imageSize.textContent = formatBytes(item.image_size);
    followup.hidden = key !== 'p4-pico';
    prompt.hidden = true;
    area.hidden = false;
    showCompatibility();
    if (scroll) document.getElementById('install-section').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  boards.forEach((button) => button.addEventListener('click', () => choose(button.dataset.target)));
  document.getElementById('select-c6').addEventListener('click', () => choose('c6-external', true));

  async function initialize() {
    try {
      const res = await fetch('./firmware/catalog.json', { cache: 'no-cache' });
      if (!res.ok) throw new Error(`Firmware catalog returned HTTP ${res.status}`);
      const data = await res.json();
      if (data.schema_version !== 1 || !Array.isArray(data.firmwares)) throw new Error('Firmware catalog is invalid.');
      catalog = Object.fromEntries(data.firmwares.map((item) => [item.key, item]));
      if (['p4-pico', 'p4-pico-wifi', 'c6-external'].some((key) => !catalog[key])) {
        throw new Error('The required three firmware variants are not present.');
      }
      available = true;
      showCompatibility();
    } catch (error) {
      available = false;
      catalog = null;
      prompt.textContent = 'Firmware files are not available. Please reload the GitHub Pages site or contact the maintainer.';
      prompt.classList.add('error-text');
      compat.hidden = false;
      compat.textContent = `Setup error: ${error.message}`;
      boards.forEach((button) => { button.disabled = true; });
    }
  }

  // Show the correct compatibility guidance even before a board is selected.
  showCompatibility();
  initialize();
})();
