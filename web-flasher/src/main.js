import JSZip from 'jszip';
import { DFU } from 'webdfu';
import './style.css';

const releaseSelect = document.querySelector('#releaseSelect');
const releaseStatus = document.querySelector('#releaseStatus');
const onlineStatus = document.querySelector('#onlineStatus');
const releaseDetails = document.querySelector('#releaseDetails');
const flashButton = document.querySelector('#flashButton');
const connectionStatus = document.querySelector('#connectionStatus');
const flashProgress = document.querySelector('#flashProgress');
const progressStatus = document.querySelector('#progressStatus');
const logElement = document.querySelector('#log');

let selectedRelease = null;

function log(message) {
  logElement.textContent += `${message}\n`;
  logElement.scrollTop = logElement.scrollHeight;
}

function isExpectedManifestResetError(error) {
  // Some devices reset after manifestation and WebUSB reports that reset as a transfer error.
  const message = error instanceof Error ? error.message : String(error);
  return message.includes('reset for manifestation') &&
    message.includes('Unable to reset the device');
}

function setDetails(manifest, file) {
  releaseDetails.innerHTML = '';
  for (const [label, value] of [
    ['File', file.name],
    ['Version', manifest.version],
    ['Board', manifest.board],
    ['Images', manifest.files.length]
  ]) {
    const term = document.createElement('dt');
    term.textContent = label;
    const description = document.createElement('dd');
    description.textContent = value;
    releaseDetails.append(term, description);
  }
  releaseDetails.hidden = false;
}

async function readRelease(file) {
  // Validate the manifest and load its images before requesting USB access.
  const zip = await JSZip.loadAsync(file);
  const manifestEntry = zip.file('manifest.json');
  if (!manifestEntry) throw new Error('manifest.json is missing from the release ZIP.');

  const manifest = JSON.parse(await manifestEntry.async('string'));
  if (!manifest.version || !Array.isArray(manifest.files) || manifest.files.length === 0) {
    throw new Error('The release manifest is incomplete.');
  }

  const images = [];
  for (const entry of manifest.files) {
    if (!entry.name || (manifest.protocol !== 'dfu' && entry.address === undefined)) {
      throw new Error('A release manifest entry is incomplete.');
    }
    const image = zip.file(entry.name);
    if (!image) throw new Error(`File is missing from the ZIP: ${entry.name}`);
    images.push({
      data: await image.async('uint8array'),
      address: entry.address === undefined ? undefined : Number(entry.address)
    });
  }
  return { manifest, images };
}

async function chooseRelease(file, fileName) {
  selectedRelease = null;
  flashButton.disabled = true;
  releaseDetails.hidden = true;
  releaseStatus.textContent = 'Checking release ...';
  try {
    const release = await readRelease(file);
    selectedRelease = release;
    setDetails(release.manifest, { name: fileName });
    releaseStatus.textContent = 'Release ready to flash.';
    flashButton.disabled = !('usb' in navigator);
    log(`Release ${release.manifest.version} loaded.`);
  } catch (error) {
    releaseStatus.textContent = error.message;
    log(`Error: ${error.message}`);
  }
}

async function loadSelectedRelease(option) {
  if (!option?.value) return;
  // Release URLs are kept relative so the same build works from any deployment directory.
  onlineStatus.textContent = 'Loading release from the web server ...';
  try {
    const response = await fetch(option.value, { cache: 'no-store' });
    if (!response.ok) throw new Error(`Release could not be loaded (${response.status}).`);
    await chooseRelease(await response.blob(), option.dataset.label || option.textContent);
    onlineStatus.textContent = 'Online release loaded.';
  } catch (error) {
    onlineStatus.textContent = error.message;
  }
}

releaseSelect.addEventListener('change', async () => {
  await loadSelectedRelease(releaseSelect.selectedOptions[0]);
});

async function loadOnlineReleases() {
  try {
    // The manifest list is fetched relative to the page, including on a hosted subpath.
    const response = await fetch('./releases.json', { cache: 'no-store' });
    if (!response.ok) throw new Error('Release list is not available.');
    const releases = (await response.json()).sort((left, right) =>
      right.version.localeCompare(left.version, undefined, { numeric: true }));
    for (const release of releases) {
      const option = document.createElement('option');
      option.value = release.url;
      option.dataset.label = release.label || release.version;
      option.textContent = `${option.dataset.label} ${release.version}`;
      releaseSelect.append(option);
    }
    onlineStatus.textContent = `${releases.length} online release(s) available.`;
    if (releases.length > 0) {
      // Default to the newest release while keeping older versions selectable.
      releaseSelect.selectedIndex = 0;
      await loadSelectedRelease(releaseSelect.selectedOptions[0]);
    }
  } catch (error) {
    onlineStatus.textContent = error.message;
  }
}

loadOnlineReleases();

flashButton.addEventListener('click', async () => {
  if (!selectedRelease) return;
  flashButton.disabled = true;
  releaseSelect.disabled = true;
  flashProgress.value = 0;
  flashProgress.hidden = false;
  progressStatus.hidden = false;
  progressStatus.textContent = 'Preparing ...';
  logElement.textContent = '';

  let device;
  let dfu;
  try {
    log('Select the USB DFU device ...');
    device = await navigator.usb.requestDevice({
      filters: [{ vendorId: 0x2341, productId: 0x0070 }]
    });
    const interfaces = DFU.findDeviceDfuInterfaces(device);
    if (interfaces.length === 0) {
      throw new Error('No DFU interface found. The board is not in USB DFU mode.');
    }
    dfu = new DFU.Device(device, interfaces[0]);
    dfu.logProgress = (done, total) => {
      if (total === undefined) {
        log(String(done));
        return;
      }
      const percent = Math.min(100, Math.round((done / total) * 100));
      flashProgress.value = percent;
      progressStatus.textContent = `Writing firmware: ${percent}%`;
      log(`Progress: ${done} / ${total}`);
    };
    connectionStatus.textContent = 'Connected. Writing firmware over USB DFU ...';
    await dfu.open();
    try {
      // The Nano ESP32 DFU interface uses 2048-byte blocks; the manifest supplies the image.
      await dfu.do_download(2048, selectedRelease.images[0].data.buffer, true);
    } catch (error) {
      if (!isExpectedManifestResetError(error)) {
        throw error;
      }
      log('Firmware transfer complete; the device already handled the USB reset.');
    }
    await dfu.close();
    flashProgress.value = 100;
    progressStatus.textContent = 'Firmware written successfully.';
    connectionStatus.textContent = 'Flash completed successfully.';
    log('Done. The board can now restart.');
  } catch (error) {
    progressStatus.textContent = 'Flashing failed.';
    connectionStatus.textContent = 'Flashing failed.';
    log(`Error: ${error.message}`);
  } finally {
    // Close on both success and failure, including resets reported after a completed transfer.
    try { await dfu?.close(); } catch { }
    flashButton.disabled = false;
    releaseSelect.disabled = false;
  }
});

if (!('usb' in navigator)) {
  connectionStatus.textContent = 'WebUSB is not supported by this browser.';
}
