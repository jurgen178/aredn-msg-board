#pragma once

#include <Arduino.h>

// Store the complete UI in flash; the firmware can serve it without a filesystem asset.
static const char BOARD_PAGE[] PROGMEM = R"AREDNHTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#173b33">
  <title>AREDN Message Board</title>
  <style>
    :root {
      color-scheme: light;
      --ink: #172722;
      --muted: #5c6e66;
      --paper: #f3f5ef;
      --surface: #ffffff;
      --line: #d7ded7;
      --green: #173b33;
      --green-light: #e4eee8;
      --orange: #c55b36;
      --danger: #a2382d;
      font-family: "Trebuchet MS", "Segoe UI", sans-serif;
      font-synthesis: none;
      text-rendering: optimizeLegibility;
    }

    * { box-sizing: border-box; }
    [hidden] { display: none !important; }

    body {
      margin: 0;
      color: var(--ink);
      background-color: var(--paper);
      background-image: radial-gradient(#cbd5cc 0.65px, transparent 0.65px);
      background-size: 16px 16px;
      min-width: 320px;
    }

    .topbar {
      color: #f6f7f2;
      background: var(--green);
      border-bottom: 5px solid var(--orange);
    }

    .topbar-inner {
      width: min(1120px, 100%);
      min-height: 176px;
      margin: 0 auto;
      padding: 28px 28px 26px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 24px;
    }

    .eyebrow {
      margin: 0 0 8px;
      color: #c8d9ce;
      font-size: 0.76rem;
      font-weight: 700;
      text-transform: uppercase;
    }

    h1 {
      margin: 0;
      font-size: 2.4rem;
      line-height: 1.1;
      font-weight: 700;
    }

    .intro {
      margin: 9px 0 0;
      color: #d8e3dc;
      max-width: 48ch;
      line-height: 1.45;
    }

    .experimental-note {
      margin: 8px 0 0;
      color: #e2a34c;
      font-size: 0.72rem;
      line-height: 1.35;
    }

    .network-state {
      display: flex;
      align-items: center;
      gap: 10px;
      flex: 0 0 auto;
      color: #e4eee8;
      font-size: 0.9rem;
    }

    .header-indicators { display: grid; justify-items: end; gap: 12px; }
    .device-clock { display: grid; justify-items: end; gap: 4px; text-align: right; }
    .device-clock time {
      min-width: 9ch;
      color: #fff;
      font-size: 2.25rem;
      font-variant-numeric: tabular-nums;
      font-weight: 700;
      line-height: 1;
    }
    .device-clock-date { color: #d8e3dc; font-size: 0.86rem; }
    .device-clock-source { color: #c8d9ce; font-size: 0.68rem; font-weight: 700; }

    .state-dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: #e2a34c;
      box-shadow: 0 0 0 4px rgb(255 255 255 / 12%);
    }

    .state-dot.online { background: #8ed1a2; }
    .state-dot.offline { background: #e78372; }

    main {
      width: min(1120px, 100%);
      margin: 0 auto;
      padding: 32px 28px 24px;
      display: grid;
      grid-template-columns: minmax(280px, 0.82fr) minmax(0, 1.18fr);
      align-items: start;
      gap: 44px;
    }

    section { min-width: 0; }

    .section-head {
      min-height: 48px;
      margin-bottom: 20px;
      display: flex;
      align-items: center;
      gap: 12px;
      border-bottom: 1px solid var(--line);
    }

    .section-mark {
      color: var(--orange);
      font-size: 0.75rem;
      font-weight: 800;
    }

    h2 {
      margin: 0;
      font-size: 1.22rem;
      line-height: 1.25;
    }

    label {
      display: block;
      margin: 16px 0 7px;
      font-size: 0.9rem;
      font-weight: 700;
    }

    input, textarea, select {
      display: block;
      width: 100%;
      border: 1px solid #aebdb3;
      border-radius: 5px;
      padding: 12px 13px;
      color: var(--ink);
      background: var(--surface);
      font: inherit;
      font-size: 1rem;
    }

    input { min-height: 48px; }

    textarea {
      min-height: 138px;
      resize: vertical;
      line-height: 1.5;
    }

    input:focus, textarea:focus, select:focus, button:focus-visible {
      outline: 3px solid #d9945e;
      outline-offset: 2px;
      border-color: var(--orange);
    }

    .field-note {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      margin-top: 6px;
      color: var(--muted);
      font-size: 0.78rem;
    }

    button {
      min-height: 46px;
      border: 0;
      border-radius: 5px;
      padding: 0 18px;
      color: #fff;
      background: var(--green);
      font: inherit;
      font-weight: 700;
      cursor: pointer;
    }

    button:hover:not(:disabled) { background: #24564a; }
    button:disabled { cursor: wait; opacity: 0.78; }

    .send-button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 10px;
      margin-top: 18px;
    }

    .spinner {
      width: 16px;
      height: 16px;
      border: 2px solid rgb(255 255 255 / 38%);
      border-top-color: #fff;
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }

    .spinner[hidden] { display: none; }

    @keyframes spin { to { transform: rotate(360deg); } }

    .form-status {
      min-height: 44px;
      margin: 12px 0 0;
      color: var(--muted);
      font-size: 0.9rem;
      line-height: 1.45;
    }

    .form-status.error { color: var(--danger); }

    .clock-setup {
      margin: 0 0 20px;
      padding: 0 0 18px;
      border-bottom: 1px solid var(--line);
    }

    .clock-setup[hidden] { display: none; }
    .clock-status { margin: 0 0 10px; color: var(--muted); font-size: 0.88rem; line-height: 1.45; }
    .clock-controls { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 8px; align-items: end; }
    .clock-controls label { grid-column: 1 / -1; margin: 0; }
    .clock-controls input { min-width: 0; }
    .clock-controls button { min-height: 48px; }

    .feed-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
    }

    .section-head.feed-head { flex-wrap: wrap; }
    .feed-title { display: flex; align-items: center; gap: 12px; }
    .board-actions { display: flex; gap: 8px; margin-left: auto; }

    .board-action-button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-height: 38px;
      padding: 0 12px;
      color: var(--green);
      background: var(--green-light);
      font-size: 0.86rem;
      text-decoration: none;
    }

    .board-action-button[hidden] { display: none; }

    .board-action-button:hover:not(:disabled) { background: #d3e3d8; }

    .feed-info {
      min-height: 24px;
      margin: -7px 0 12px;
      color: var(--muted);
      font-size: 0.82rem;
    }

    .message-search {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto auto auto;
      gap: 8px;
      margin: 0 0 14px;
    }

    .message-search input { min-width: 0; }
    .message-search select { min-width: 130px; }
    .message-search button { min-height: 42px; padding: 0 13px; font-size: 0.86rem; }

    .message-list {
      display: grid;
      gap: 10px;
    }

    .message {
      padding: 15px 16px 16px;
      border: 1px solid var(--line);
      border-left: 4px solid #759985;
      border-radius: 5px;
      background: var(--surface);
      overflow-wrap: anywhere;
    }

    .message.priority-green { border-left-color: #759985; }
    .message.priority-orange { border-left-color: #e68a00; }
    .message.priority-red { border-left-color: #c62828; }

    .message-meta {
      padding: 6px 8px;
      border-radius: 4px;
      background: var(--green-light);
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      gap: 12px;
      margin-bottom: 8px;
    }

    .message.priority-orange .message-meta { background: #fff0cf; }
    .message.priority-red .message-meta { background: #ffe5e5; }

    .message-name { font-weight: 700; }
    .message-priority {
      display: inline-block;
      padding: 2px 6px;
      border-radius: 3px;
      font-size: 0.7rem;
      font-weight: 700;
      text-transform: uppercase;
    }
    .message-priority-green { color: #216e39; background: #dff3e4; }
    .message-priority-orange { color: #8a4b00; background: #ffe7b3; }
    .message-priority-red { color: #a61b1b; background: #ffd6d6; }
    .message-age { color: var(--muted); font-size: 0.78rem; white-space: nowrap; }
    .message-text { margin: 0; line-height: 1.5; overflow-wrap: anywhere; }
    .message-text p { margin: 0 0 10px; }
    .message-text p:last-child { margin-bottom: 0; }
    .message-text h1,
    .message-text h2,
    .message-text h3 { margin: 0 0 8px; line-height: 1.25; }
    .message-text h1 { font-size: 1.35rem; }
    .message-text h2 { font-size: 1.2rem; }
    .message-text h3 { font-size: 1.08rem; }
    .message-text ul { margin: 0 0 10px; padding-left: 24px; }
    .message-text li { padding-left: 3px; }
    .message-text code { padding: 1px 4px; border-radius: 3px; color: var(--green); background: var(--green-light); font: 0.9em ui-monospace, SFMono-Regular, Consolas, monospace; }
    .message-text a { color: var(--green); }

    .message-admin-actions {
      display: flex;
      gap: 8px;
      margin-top: 12px;
    }

    .message-admin-actions button,
    .admin-secondary {
      min-height: 34px;
      padding: 0 11px;
      color: var(--green);
      background: var(--green-light);
      font-size: 0.82rem;
    }

    .message-admin-actions button:hover:not(:disabled),
    .admin-secondary:hover:not(:disabled) { background: #d3e3d8; }

    .admin-panel {
      grid-column: 1 / -1;
      padding-top: 4px;
      border-top: 1px solid var(--line);
    }

    .admin-login,
    .admin-controls {
      display: flex;
      align-items: end;
      flex-wrap: wrap;
      gap: 10px;
    }

    .admin-login label { margin: 0; }
    .admin-login input { width: min(280px, 100%); }
    .admin-note { margin: 8px 0 0; color: var(--muted); font-size: 0.74rem; }
    .admin-controls[hidden], .admin-login[hidden] { display: none; }
    .admin-status { min-height: 24px; margin: 10px 0 0; color: var(--muted); font-size: 0.84rem; }
    .admin-status.error { color: var(--danger); }

    .operation-dialog {
      position: fixed;
      inset: 0;
      z-index: 10;
      display: grid;
      place-items: center;
      padding: 24px;
      background: rgb(23 39 34 / 28%);
    }

    .operation-dialog[hidden] { display: none; }

    .operation-dialog-panel {
      width: min(360px, 100%);
      padding: 24px;
      border: 1px solid var(--line);
      border-radius: 5px;
      color: var(--ink);
      background: var(--surface);
      box-shadow: 0 14px 40px rgb(23 39 34 / 20%);
      text-align: center;
    }

    .operation-dialog-spinner {
      width: 28px;
      height: 28px;
      margin: 0 auto 14px;
      border: 3px solid #c9d7cd;
      border-top-color: var(--orange);
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }

    .operation-dialog-title {
      margin: 0;
      font-size: 1.1rem;
    }

    .operation-dialog-message {
      margin: 8px 0 0;
      color: var(--muted);
      font-size: 0.9rem;
      line-height: 1.45;
    }

    .edit-dialog {
      position: fixed;
      inset: 0;
      z-index: 9;
      display: grid;
      place-items: center;
      padding: 24px;
      background: rgb(23 39 34 / 28%);
    }

    .edit-dialog[hidden] { display: none; }

    .edit-dialog-panel {
      width: min(520px, 100%);
      padding: 24px;
      border: 1px solid var(--line);
      border-radius: 5px;
      color: var(--ink);
      background: var(--surface);
      box-shadow: 0 14px 40px rgb(23 39 34 / 20%);
    }

    .edit-dialog-title { margin: 0 0 18px; font-size: 1.1rem; }
    .edit-dialog-actions {
      display: flex;
      justify-content: flex-end;
      gap: 8px;
      margin-top: 18px;
    }
    .edit-dialog-actions .admin-secondary { min-height: 46px; }

    .empty-state {
      padding: 24px 0;
      color: var(--muted);
      line-height: 1.5;
    }

    footer {
      width: min(1120px, 100%);
      margin: 0 auto;
      padding: 17px 28px 26px;
      border-top: 1px solid var(--line);
      color: var(--muted);
      font-size: 0.8rem;
      line-height: 1.5;
    }

    @media (max-width: 780px) {
      h1 { font-size: 1.9rem; }

      .topbar-inner {
        min-height: 0;
        padding: 24px 20px 22px;
        align-items: flex-start;
        flex-direction: column;
        gap: 15px;
      }

      .header-indicators {
        width: 100%;
        display: flex;
        align-items: center;
        justify-content: space-between;
      }

      .device-clock { justify-items: start; text-align: left; }
      .device-clock time { font-size: 1.85rem; }

      main {
        padding: 24px 20px 20px;
        grid-template-columns: minmax(0, 1fr);
        gap: 34px;
      }

      footer { padding: 16px 20px 24px; }
      .send-button { width: 100%; }
      .clock-controls { grid-template-columns: minmax(0, 1fr); }
      .message-search { grid-template-columns: minmax(0, 1fr) auto auto; }
      .message-search input { grid-column: 1 / -1; }
    }

    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        scroll-behavior: auto !important;
        animation-duration: 0.01ms !important;
        animation-iteration-count: 1 !important;
      }
    }
  </style>
</head>
<body>
  <div id="operationDialog" class="operation-dialog" role="dialog" aria-modal="true" aria-labelledby="operationDialogTitle" hidden>
    <div class="operation-dialog-panel">
      <div class="operation-dialog-spinner" aria-hidden="true"></div>
      <h2 id="operationDialogTitle" class="operation-dialog-title">Working ...</h2>
      <p id="operationDialogMessage" class="operation-dialog-message">The board is saving your change.</p>
    </div>
  </div>
  <div id="editDialog" class="edit-dialog" role="dialog" aria-modal="true" aria-labelledby="editDialogTitle" hidden>
    <form id="editForm" class="edit-dialog-panel">
      <h2 id="editDialogTitle" class="edit-dialog-title">Edit message</h2>
      <label for="editName">Name or callsign</label>
      <input id="editName" maxlength="32" required>
      <label for="editText">Message</label>
      <textarea id="editText" maxlength="1024" rows="5" required></textarea>
      <label for="editPriority">Priority</label>
      <select id="editPriority">
        <option value="green">Green · normal</option>
        <option value="orange">Orange · important</option>
        <option value="red">Red · urgent</option>
      </select>
      <div class="edit-dialog-actions">
        <button id="editCancelButton" class="admin-secondary" type="button">Cancel</button>
        <button type="submit">Save</button>
      </div>
    </form>
  </div>
  <header class="topbar">
    <div class="topbar-inner">
      <div>
        <p class="eyebrow">AREDN · Local message service</p>
        <h1>Message Board</h1>
        <p class="intro">Short updates for everyone on the network.</p>
        <p class="experimental-note">EXPERIMENTAL ESP32-S3 WEB SERVICE · RESOURCE-CONSCIOUS HTTP POLLING</p>
      </div>
      <div class="header-indicators">
        <div class="device-clock" aria-label="Device clock">
          <time id="deviceClock">TIME NOT SET</time>
          <span id="deviceClockDate" class="device-clock-date">Awaiting network time</span>
          <span id="deviceClockSource" class="device-clock-source">CLOCK STATUS UNKNOWN</span>
        </div>
        <div class="network-state" aria-live="polite">
          <span id="networkDot" class="state-dot" aria-hidden="true"></span>
          <span id="networkText">Checking service</span>
        </div>
      </div>
    </div>
  </header>

  <main>
    <section aria-labelledby="composeTitle">
      <div class="section-head">
        <span class="section-mark">01</span>
        <h2 id="composeTitle">Write a message</h2>
      </div>
      <div id="clockSetup" class="clock-setup" hidden>
        <p id="clockStatus" class="clock-status" role="status" aria-live="polite">Checking for network time ...</p>
        <form id="clockForm">
          <div class="clock-controls">
            <label for="deviceDateTime">Set device date and time</label>
            <input id="deviceDateTime" type="datetime-local" required>
            <button type="submit">Set device time</button>
          </div>
        </form>
      </div>
      <form id="messageForm">
        <label for="author">Name or callsign</label>
        <input id="author" name="name" maxlength="32" autocomplete="nickname" required>

        <label for="messageText">Message</label>
        <textarea id="messageText" name="text" maxlength="1024" rows="5" required></textarea>
        <div class="field-note">
          <span>Up to 1024 characters</span>
          <span id="characterCount">0 / 1024</span>
        </div>
        <label for="messagePriority">Priority</label>
        <select id="messagePriority" name="priority">
          <option value="green">Green · normal</option>
          <option value="orange">Orange · important</option>
          <option value="red">Red · urgent</option>
        </select>
        <p class="admin-note">Optional formatting: <code># heading</code>, <code>**bold**</code>, <code>*italic*</code>, <code>- list</code></p>

        <button id="sendButton" class="send-button" type="submit">
          <span id="sendSpinner" class="spinner" aria-hidden="true" hidden></span>
          <span id="sendLabel">Post message</span>
        </button>
        <p id="formStatus" class="form-status" role="status" aria-live="polite"></p>
      </form>
    </section>

    <section aria-labelledby="boardTitle">
      <div class="section-head feed-head">
        <div class="feed-title">
          <span class="section-mark">02</span>
          <h2 id="boardTitle">Latest messages</h2>
        </div>
        <div class="board-actions">
          <a id="exportButton" class="board-action-button" href="/api/messages/export.json" download="aredn-messages.json" title="Download all stored messages as JSON">Export Messages</a>
        </div>
      </div>
      <p id="feedInfo" class="feed-info" aria-live="polite">Loading messages ...</p>
      <form id="searchForm" class="message-search">
        <input id="searchInput" type="search" maxlength="64" placeholder="Search name or message" aria-label="Search name or message">
        <select id="priorityFilter" aria-label="Filter by priority">
          <option value="all">All priorities</option>
          <option value="green">Green</option>
          <option value="orange">Orange</option>
          <option value="red">Red</option>
        </select>
        <button type="submit">Search</button>
        <button id="clearSearchButton" class="admin-secondary" type="button">Clear</button>
      </form>
      <div id="messageList" class="message-list" aria-live="polite"></div>
      <button id="loadMoreButton" class="board-action-button" type="button" hidden>Load more</button>
      <p id="emptyState" class="empty-state" hidden>No messages yet. Post the first one.</p>
    </section>

    <section class="admin-panel" aria-labelledby="adminTitle">
      <div class="section-head">
        <span class="section-mark">03</span>
        <h2 id="adminTitle">Administrator</h2>
      </div>
      <form id="adminLoginForm" class="admin-login">
        <label for="adminPassword">Password</label>
        <input id="adminPassword" type="password" autocomplete="current-password" required>
        <button type="submit">Sign in</button>
      </form>
      <p class="admin-note">For authorized operators. Ask the local deployment contact for access. Login helps prevent unintended changes.</p>
      <div id="adminControls" class="admin-controls" hidden>
        <button id="resetButton" type="button">Reset all messages</button>
        <button id="importButton" class="admin-secondary" type="button">Import Messages</button>
        <input id="importFile" type="file" accept="application/json,.json" hidden>
        <button id="logoutButton" class="admin-secondary" type="button">Sign out</button>
      </div>
      <p id="adminStatus" class="admin-status" role="status" aria-live="polite"></p>
    </section>
  </main>

  <script>
    const form = document.getElementById('messageForm');
    const authorInput = document.getElementById('author');
    const messageInput = document.getElementById('messageText');
    const messagePriorityInput = document.getElementById('messagePriority');
    const sendButton = document.getElementById('sendButton');
    const sendSpinner = document.getElementById('sendSpinner');
    const sendLabel = document.getElementById('sendLabel');
    const formStatus = document.getElementById('formStatus');
    const messageList = document.getElementById('messageList');
    const emptyState = document.getElementById('emptyState');
    const feedInfo = document.getElementById('feedInfo');
    const searchForm = document.getElementById('searchForm');
    const searchInput = document.getElementById('searchInput');
    const priorityFilter = document.getElementById('priorityFilter');
    const clearSearchButton = document.getElementById('clearSearchButton');
    const loadMoreButton = document.getElementById('loadMoreButton');
    const networkDot = document.getElementById('networkDot');
    const networkText = document.getElementById('networkText');
    const clockSetup = document.getElementById('clockSetup');
    const clockStatus = document.getElementById('clockStatus');
    const clockForm = document.getElementById('clockForm');
    const deviceDateTime = document.getElementById('deviceDateTime');
    const deviceClock = document.getElementById('deviceClock');
    const deviceClockDate = document.getElementById('deviceClockDate');
    const deviceClockSource = document.getElementById('deviceClockSource');
    const adminLoginForm = document.getElementById('adminLoginForm');
    const adminPassword = document.getElementById('adminPassword');
    const adminControls = document.getElementById('adminControls');
    const adminStatus = document.getElementById('adminStatus');
    const importButton = document.getElementById('importButton');
    const importFile = document.getElementById('importFile');
    const exportButton = document.getElementById('exportButton');
    const operationDialog = document.getElementById('operationDialog');
    const operationDialogTitle = document.getElementById('operationDialogTitle');
    const operationDialogMessage = document.getElementById('operationDialogMessage');
    const editDialog = document.getElementById('editDialog');
    const editForm = document.getElementById('editForm');
    const editName = document.getElementById('editName');
    const editText = document.getElementById('editText');
    const editPriority = document.getElementById('editPriority');
    const editCancelButton = document.getElementById('editCancelButton');

    let acceptedId = null;
    let retryCount = 0;
    let retryTimer = 0;
    let refreshInProgress = false;
    let deviceClockSet = false;
    let messageSending = false;
    let adminAuthenticated = false;
    let adminStateRequestId = 0;
    let sendButtonLabel = 'Post message';
    let clockEpochMs = 0;
    let clockReceivedAt = 0;
    let loadedMessages = [];
    let latestMessageId = 0;
    let oldestMessageId = 0;
    let moreMessagesAvailable = false;
    let boardRevision = null;
    let refreshRequested = false;
    let requestedRefreshMode = 'after';
    let viewGeneration = 0;
    let searchActive = false;
    let searchTerm = '';
    let selectedPriority = 'all';
    let editDialogResolve = null;
    const POLL_INTERVAL_MS = 30000;
    const POLL_JITTER_MS = 3000;
    const POLL_MIN_DELAY_MS = 1000;
    let pollTimer = 0;
    sendButton.disabled = true;

    function showOperationDialog(title, message) {
      operationDialogTitle.textContent = title;
      operationDialogMessage.textContent = message;
      operationDialog.hidden = false;
    }

    function hideOperationDialog() {
      operationDialog.hidden = true;
    }

    function uploadImportFile(file, totalMessages) {
      return new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        request.open('POST', '/api/admin/import');
        request.setRequestHeader('Content-Type', 'application/json');
        request.upload.addEventListener('progress', (event) => {
          if (!event.lengthComputable || totalMessages === 0) return;
          const uploadedMessages = Math.min(
            totalMessages,
            Math.floor((event.loaded / event.total) * totalMessages));
          operationDialogMessage.textContent =
            `Importing messages: ${uploadedMessages} / ${totalMessages}`;
        });
        request.upload.addEventListener('load', () => {
          if (totalMessages > 0) {
            operationDialogMessage.textContent = `Importing messages: ${totalMessages} / ${totalMessages}`;
          }
          window.setTimeout(() => {
            operationDialogMessage.textContent = 'Import is being processed ...';
          }, 0);
        });
        request.addEventListener('load', () => {
          resolve(request);
        });
        request.addEventListener('error', () => {
          reject(new Error('Import upload failed'));
        });
        request.send(file);
      });
    }

    async function exportMessages(event) {
      event.preventDefault();
      showOperationDialog('Exporting messages', 'The board is preparing the complete message archive.');
      try {
        const messages = [];
        let beforeId = 0;
        let storedCount = 0;
        let hasMore = true;
        while (hasMore) {
          const endpoint = beforeId === 0
            ? '/api/messages?limit=100'
            : `/api/messages?before=${beforeId}&limit=100`;
          const response = await fetch(endpoint, { cache: 'no-store' });
          if (!response.ok) {
            throw new Error('Export page failed');
          }
          const page = await response.json();
          storedCount = page.c;
          messages.push(...page.p.map((message) => {
            const exportedMessage = {
              id: message.i,
              created_at: new Date(message.t * 1000).toISOString(),
              name: message.u,
              text: message.m
            };
            if (message.p && message.p !== 'green') {
              exportedMessage.priority = message.p;
            }
            return exportedMessage;
          }));
          hasMore = page.h;
          if (hasMore) {
            const lastMessage = page.p[page.p.length - 1];
            if (!lastMessage) {
              throw new Error('Export pagination failed');
            }
            beforeId = lastMessage.i;
            operationDialogMessage.textContent =
              `Preparing ${messages.length} of ${storedCount} messages ...`;
          }
        }
        const archive = new Blob([JSON.stringify({
          format: 'aredn-message-board',
          message_count: storedCount,
          messages
        }, null, 2) + '\n'], { type: 'application/json' });
        const downloadUrl = URL.createObjectURL(archive);
        const download = document.createElement('a');
        download.href = downloadUrl;
        download.download = 'aredn-messages.json';
        document.body.append(download);
        download.click();
        download.remove();
        window.setTimeout(() => URL.revokeObjectURL(downloadUrl), 1000);
      } catch (error) {
        setAdminError('Export failed. Check the connection and try again.');
      } finally {
        hideOperationDialog();
      }
    }

    function setBusy(busy, label) {
      messageSending = busy;
      sendButton.disabled = busy;
      authorInput.disabled = busy;
      messageInput.disabled = busy;
      messagePriorityInput.disabled = busy;
      sendSpinner.hidden = !busy;
      sendButtonLabel = label;
      sendLabel.textContent = sendButtonLabel;
    }

    function setFormStatus(text, isError = false) {
      formStatus.textContent = text;
      formStatus.classList.toggle('error', isError);
    }

    function setAdminState(authenticated, statusText = '') {
      adminAuthenticated = authenticated;
      adminLoginForm.hidden = authenticated;
      adminControls.hidden = !authenticated;
      clockSetup.hidden = !authenticated;
      adminStatus.textContent = statusText;
      adminStatus.classList.remove('error');
    }

    function setAdminError(text) {
      adminStatus.textContent = text;
      adminStatus.classList.add('error');
    }

    function setNetworkState(online) {
      networkDot.classList.toggle('online', online);
      networkDot.classList.toggle('offline', !online);
      networkText.textContent = online ? 'Service online' : 'Connection lost';
    }

    function renderHeaderClock() {
      if (!deviceClockSet || clockEpochMs === 0) {
        deviceClock.textContent = 'TIME NOT SET';
        deviceClock.removeAttribute('datetime');
        return;
      }

      const currentDate = new Date(clockEpochMs + performance.now() - clockReceivedAt);
      if (Number.isNaN(currentDate.getTime())) {
        deviceClock.textContent = 'TIME NOT SET';
        deviceClock.removeAttribute('datetime');
        return;
      }

      deviceClock.dateTime = currentDate.toISOString();
      deviceClock.textContent = currentDate.toLocaleTimeString('en-GB', {
        hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false
      });
      deviceClockDate.textContent = currentDate.toLocaleDateString('en-GB', {
        weekday: 'long', day: '2-digit', month: 'long', year: 'numeric'
      });
    }

    function updateClockState(isSet, source, epoch) {
      deviceClockSet = isSet && Number.isFinite(epoch) && epoch > 0;
      sendButton.disabled = messageSending;

      if (deviceClockSet) {
        clockEpochMs = epoch * 1000;
        clockReceivedAt = performance.now();
      } else {
        clockEpochMs = 0;
      }
      renderHeaderClock();

      if (deviceClockSet && source === 'client') {
        deviceClockSource.textContent = 'MESH TIME';
        clockStatus.textContent = 'Device clock synchronized from AREDN mesh clients.';
      } else if (deviceClockSet && source === 'network') {
        deviceClockSource.textContent = 'NETWORK TIME';
        clockStatus.textContent = 'Device clock synchronized with network time.';
      } else if (deviceClockSet) {
        deviceClockSource.textContent = 'MANUALLY SET';
        clockStatus.textContent = 'Device time was set manually. It will be checked for network time again; set it again after a restart if needed.';
      } else {
        deviceClockSource.textContent = 'TIME NOT SET';
        deviceClockDate.textContent = 'Awaiting network time';
        clockStatus.textContent = 'Network time is unavailable. An administrator can set the device date and time below.';
      }
    }

    function formatLocalDateTime(date) {
      const localDate = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
      return localDate.toISOString().slice(0, 16);
    }

    function appendMarkdownInline(parent, source) {
      const tokenPattern = /(\*\*[^*\n]+\*\*|\*[^*\n]+\*|`[^`\n]+`|\[[^\]\n]+\]\(https?:\/\/[^\s)]+\)|https?:\/\/[^\s<]+)/g;
      let position = 0;
      for (const match of source.matchAll(tokenPattern)) {
        const token = match[0];
        const start = match.index;
        if (start > position) {
          parent.append(document.createTextNode(source.slice(position, start)));
        }
        if (token.startsWith('**') && token.endsWith('**')) {
          const element = document.createElement('strong');
          element.textContent = token.slice(2, -2);
          parent.append(element);
        } else if (token.startsWith('*') && token.endsWith('*')) {
          const element = document.createElement('em');
          element.textContent = token.slice(1, -1);
          parent.append(element);
        } else if (token.startsWith('`') && token.endsWith('`')) {
          const element = document.createElement('code');
          element.textContent = token.slice(1, -1);
          parent.append(element);
        } else {
          const linkMatch = token.match(/^\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)$/);
          const url = linkMatch === null ? token : linkMatch[2];
          const element = document.createElement('a');
          element.href = url;
          element.target = '_blank';
          element.rel = 'noopener noreferrer';
          element.textContent = linkMatch === null ? url : linkMatch[1];
          parent.append(element);
        }
        position = start + token.length;
      }
      if (position < source.length) {
        parent.append(document.createTextNode(source.slice(position)));
      }
    }

    function renderMarkdown(source) {
      const fragment = document.createDocumentFragment();
      const lines = source.replace(/\r\n?/g, '\n').split('\n');
      let paragraph = null;
      let list = null;

      const finishParagraph = () => {
        paragraph = null;
      };

      const finishList = () => {
        list = null;
      };

      for (const line of lines) {
        const headingMatch = line.match(/^(#{1,3})\s+(.+)$/);
        const listMatch = line.match(/^\s*-\s+(.+)$/);
        if (headingMatch !== null) {
          finishParagraph();
          finishList();
          const heading = document.createElement(`h${headingMatch[1].length}`);
          appendMarkdownInline(heading, headingMatch[2]);
          fragment.append(heading);
        } else if (listMatch !== null) {
          finishParagraph();
          if (list === null) {
            list = document.createElement('ul');
            fragment.append(list);
          }
          const item = document.createElement('li');
          appendMarkdownInline(item, listMatch[1]);
          list.append(item);
        } else if (line.trim() === '') {
          finishParagraph();
          finishList();
        } else {
          finishList();
          if (paragraph === null) {
            paragraph = document.createElement('p');
            fragment.append(paragraph);
          } else {
            paragraph.append(document.createElement('br'));
          }
          appendMarkdownInline(paragraph, line);
        }
      }
      return fragment;
    }

    function renderMessages(messages) {
      messageList.replaceChildren();
      emptyState.hidden = messages.length !== 0;

      for (const message of messages) {
        const article = document.createElement('article');
        const priority = message.p || 'green';
        article.className = `message priority-${priority}`;

        const meta = document.createElement('div');
        meta.className = 'message-meta';
        const author = document.createElement('span');
        author.className = 'message-name';
        author.textContent = message.u;
        if (priority !== 'green') {
          const priorityLabel = document.createElement('span');
          priorityLabel.className = `message-priority message-priority-${priority}`;
          priorityLabel.textContent = priority;
          priorityLabel.setAttribute('aria-label', `Priority ${priority}`);
          author.append(' · ', priorityLabel);
        }
        const age = document.createElement('time');
        age.className = 'message-age';
        const messageDate = new Date(message.t * 1000);
        age.dateTime = messageDate.toISOString();
        age.textContent = messageDate.toLocaleString();
        meta.append(author, age);

        const text = document.createElement('div');
        text.className = 'message-text';
        text.append(renderMarkdown(message.m));
        article.append(meta, text);
        if (adminAuthenticated) {
          const actions = document.createElement('div');
          actions.className = 'message-admin-actions';
          const editButton = document.createElement('button');
          editButton.type = 'button';
          editButton.textContent = 'Edit';
          editButton.addEventListener('click', () => editMessage(message));
          const deleteButton = document.createElement('button');
          deleteButton.type = 'button';
          deleteButton.textContent = 'Delete';
          deleteButton.addEventListener('click', () => deleteMessage(message.i));
          actions.append(editButton, deleteButton);
          article.append(actions);
        }
        messageList.append(article);
      }
    }

    function mergeMessages(messages, replaceAll) {
      const messageMap = new Map(replaceAll ? [] : loadedMessages.map((message) => [message.i, message]));
      for (const message of messages) {
        messageMap.set(message.i, message);
      }
      loadedMessages = [...messageMap.values()].sort((left, right) => right.i - left.i);
      latestMessageId = loadedMessages.length === 0 ? 0 : loadedMessages[0].i;
      oldestMessageId = loadedMessages.length === 0
        ? 0
        : loadedMessages[loadedMessages.length - 1].i;
      renderMessages(loadedMessages);
    }

    function updateLoadMoreState() {
      loadMoreButton.hidden = !moreMessagesAvailable;
      loadMoreButton.disabled = refreshInProgress;
    }

    async function refreshAdminState() {
      const requestId = ++adminStateRequestId;
      try {
        const response = await fetch('/api/admin/status', { cache: 'no-store' });
        const data = await response.json();
        if (requestId === adminStateRequestId) {
          setAdminState(data.authenticated);
        }
      } catch (error) {
        if (requestId === adminStateRequestId) {
          setAdminState(false);
        }
      }
    }

    function openEditDialog(message) {
      editName.value = message.u;
      editText.value = message.m;
      editPriority.value = message.p || 'green';
      editDialog.hidden = false;
      editName.focus();
      return new Promise((resolve) => {
        editDialogResolve = resolve;
      });
    }

    function closeEditDialog(result) {
      editDialog.hidden = true;
      if (editDialogResolve !== null) {
        const resolve = editDialogResolve;
        editDialogResolve = null;
        resolve(result);
      }
    }

    async function editMessage(message) {
      const values = await openEditDialog(message);
      if (values === null) return;
      showOperationDialog('Saving message', 'The board is writing the updated message.');
      try {
        const response = await fetch(`/api/admin/message?id=${message.i}`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({
            name: values.name.trim(),
            text: values.text.trim(),
            priority: values.priority
          })
        });
        if (response.status === 401) {
          setAdminState(false, 'Your admin session has expired.');
          return;
        }
        if (!response.ok) {
          setAdminError('The message could not be edited.');
          return;
        }
        await refreshMessages(searchActive ? 'search' : 'full');
        adminStatus.textContent = 'Message updated.';
      } catch (error) {
        setAdminError('The message could not be edited. Check the connection.');
      } finally {
        hideOperationDialog();
      }
    }

    editForm.addEventListener('submit', (event) => {
      event.preventDefault();
      closeEditDialog({
        name: editName.value,
        text: editText.value,
        priority: editPriority.value
      });
    });

    editCancelButton.addEventListener('click', () => closeEditDialog(null));

    async function deleteMessage(id) {
      if (!window.confirm('Delete this message?')) return;
      showOperationDialog('Deleting message', 'The board is safely deleteing the message from its message store.');
      try {
        const response = await fetch(`/api/admin/message?id=${id}`, { method: 'DELETE' });
        if (response.status === 401) {
          setAdminState(false, 'Your admin session has expired.');
          return;
        }
        if (!response.ok) {
          setAdminError('The message could not be deleted.');
          return;
        }
        await refreshMessages(searchActive ? 'search' : 'full');
        adminStatus.textContent = 'Message deleted.';
      } catch (error) {
        setAdminError('The message could not be deleted. Check the connection.');
      } finally {
        hideOperationDialog();
      }
    }

    function addClientTime(endpoint) {
      const separator = endpoint.includes('?') ? '&' : '?';
      return `${endpoint}${separator}c_time=${Math.floor(Date.now() / 1000)}`;
    }

    async function refreshMessages(mode = 'after') {
      if (refreshInProgress) {
        refreshRequested = true;
        if (mode === 'latest' ||
            (requestedRefreshMode !== 'latest' && (mode === 'full' || mode === 'search'))) {
          requestedRefreshMode = mode;
        }
        return;
      }
      refreshInProgress = true;
      const requestGeneration = viewGeneration;
      updateLoadMoreState();
      try {
        const isIncremental = mode === 'after' && latestMessageId !== 0;
        const priorityQuery = selectedPriority === 'all'
          ? '' : `&priority=${encodeURIComponent(selectedPriority)}`;
        let endpoint = `/api/messages?limit=50${priorityQuery}`;
        if (mode === 'search') {
          endpoint = `/api/messages?search=${encodeURIComponent(searchTerm)}&limit=50${priorityQuery}`;
        } else if (mode === 'full') {
          endpoint = `/api/messages?limit=50${priorityQuery}`;
        } else if (isIncremental) {
          endpoint = `/api/messages?after=${latestMessageId}&limit=50${priorityQuery}`;
        } else if (mode === 'before' && oldestMessageId !== 0) {
          endpoint = `/api/messages?before=${oldestMessageId}&limit=50` +
            (searchActive ? `&search=${encodeURIComponent(searchTerm)}` : '') + priorityQuery;
        }
        const response = await fetch(addClientTime(endpoint), { cache: 'no-store' });
        if (!response.ok) throw new Error('Board unavailable');
        const data = await response.json();
        if (requestGeneration !== viewGeneration) {
          return;
        }
        const revisionChanged = boardRevision !== null && data.r !== boardRevision;
        if (revisionChanged && data.f && mode !== 'full') {
          refreshInProgress = false;
          updateLoadMoreState();
          await refreshMessages(mode === 'search' ? 'search' : 'full');
          return;
        }
        boardRevision = data.r;
        const appendPage = mode === 'before';
        mergeMessages(data.p, !appendPage &&
          (mode === 'full' || mode === 'search' || !isIncremental));
        moreMessagesAvailable = isIncremental ? moreMessagesAvailable : data.h;
        updateLoadMoreState();
        updateClockState(data.v, data.s, data.e);
        const filterLabel = selectedPriority === 'all' ? '' : ` · ${selectedPriority}`;
        feedInfo.textContent = searchActive
          ? `Search results for "${searchTerm}"${filterLabel} · ${loadedMessages.length}${data.h ? '+' : ''} matches`
          : `${loadedMessages.length} shown${filterLabel} · ${data.c} stored posts`;
        setNetworkState(true);

        if (acceptedId !== null && loadedMessages.some((message) => message.i === acceptedId)) {
          acceptedId = null;
          clearTimeout(retryTimer);
          form.reset();
          document.getElementById('characterCount').textContent = '0 / 1024';
          setBusy(false, 'Post message');
          setFormStatus('Your message is now on the board.');
        }
      } catch (error) {
        setNetworkState(false);
        feedInfo.textContent = 'Could not load messages.';
      } finally {
        const followUpRequested = refreshRequested;
        const followUpMode = requestedRefreshMode;
        refreshRequested = false;
        requestedRefreshMode = 'after';
        refreshInProgress = false;
        updateLoadMoreState();
        if (followUpRequested) {
          void refreshMessages(followUpMode);
        }
      }
    }

    function requestRefresh(mode = searchActive ? 'search' : 'after') {
      return refreshMessages(mode);
    }

    function scheduleNextPoll() {
      const jitterMs = Math.random() * 2 * POLL_JITTER_MS - POLL_JITTER_MS;
      const delayMs = Math.max(POLL_MIN_DELAY_MS, POLL_INTERVAL_MS + jitterMs);
      pollTimer = window.setTimeout(async () => {
        pollTimer = 0;
        if (!document.hidden) {
          await requestRefresh();
        }
        scheduleNextPoll();
      }, delayMs);
    }

    async function loadMoreMessages() {
      await refreshMessages('before');
    }

    function watchAcceptedMessage() {
      if (acceptedId === null) return;
      requestRefresh();
    }

    async function submitMessage() {
      try {
        const response = await fetch('/api/messages', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({
            name: authorInput.value.trim(),
            text: messageInput.value.trim(),
            priority: messagePriorityInput.value,
            c_time: String(Math.floor(Date.now() / 1000))
          })
        });

        if (response.status === 429) {
          retryCount += 1;
          const delay = Math.min(8000, 1000 * (2 ** Math.min(retryCount - 1, 3))) + Math.random() * 400;
          setBusy(true, 'Waiting for space');
          setFormStatus('Many messages at once. Your text is saved here; retrying in a few seconds.');
          retryTimer = setTimeout(submitMessage, delay);
          return;
        }

        if (response.status !== 202) {
          const data = await response.json().catch(() => ({}));
          setBusy(false, 'Try again');
          setFormStatus(data.error === 'too_long'
            ? 'The name or message is too long.'
            : data.error === 'board_full'
              ? 'The message board is full. Delete an older message before posting again.'
              : 'The message was not accepted. Your text is still in the form.', true);
          return;
        }

        const data = await response.json();
        acceptedId = data.id;
        retryCount = 0;
        setBusy(true, 'Posting');
        setFormStatus('Accepted. Waiting for your post to appear on the board.');
        watchAcceptedMessage();
      } catch (error) {
        setBusy(false, 'Try again');
        setFormStatus('No response received. Check the board before submitting again.', true);
      }
    }

    form.addEventListener('submit', (event) => {
      event.preventDefault();
      if (acceptedId !== null || messageSending) return;
      retryCount = 0;
      setBusy(true, 'Sending');
      setFormStatus('');
      submitMessage();
    });

    messageInput.addEventListener('input', () => {
      document.getElementById('characterCount').textContent = `${messageInput.value.length} / 1024`;
    });

    function resetMessageView() {
      loadedMessages = [];
      latestMessageId = 0;
      oldestMessageId = 0;
      moreMessagesAvailable = false;
      renderMessages(loadedMessages);
      updateLoadMoreState();
    }

    searchForm.addEventListener('submit', (event) => {
      event.preventDefault();
      const value = searchInput.value.trim();
      if (value.length === 0) {
        clearSearchButton.click();
        return;
      }
      searchTerm = value;
      searchActive = true;
      ++viewGeneration;
      resetMessageView();
      requestRefresh('search');
    });

    clearSearchButton.addEventListener('click', () => {
      ++viewGeneration;
      searchActive = false;
      searchTerm = '';
      searchInput.value = '';
      resetMessageView();
      requestRefresh('latest');
    });

    priorityFilter.addEventListener('change', () => {
      selectedPriority = priorityFilter.value;
      ++viewGeneration;
      resetMessageView();
      requestRefresh(searchActive ? 'search' : 'latest');
    });

    exportButton.addEventListener('click', exportMessages);
    loadMoreButton.addEventListener('click', loadMoreMessages);
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) {
        requestRefresh(searchActive ? 'search' : 'full');
      }
    });
    adminLoginForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const response = await fetch('/api/admin/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ password: adminPassword.value })
      });
      if (!response.ok) {
        setAdminError('Invalid administrator password.');
        return;
      }
      adminPassword.value = '';
      ++adminStateRequestId;
      setAdminState(true, 'Administrator mode enabled.');
      await refreshMessages();
    });

    document.getElementById('logoutButton').addEventListener('click', async () => {
      await fetch('/api/admin/logout', { method: 'POST' });
      ++adminStateRequestId;
      setAdminState(false, 'Signed out.');
      await refreshMessages();
    });

    document.getElementById('resetButton').addEventListener('click', async () => {
      if (!window.confirm('Delete all messages permanently?')) return;
      showOperationDialog('Resetting message board', 'The board is creating an empty validated store.');
      try {
        const response = await fetch('/api/admin/messages', { method: 'DELETE' });
        if (!response.ok) {
          setAdminError('The message board could not be reset.');
          return;
        }
        await refreshMessages('full');
        adminStatus.textContent = 'All messages deleted.';
      } catch (error) {
        setAdminError('The message board could not be reset. Check the connection.');
      } finally {
        hideOperationDialog();
      }
    });

    importButton.addEventListener('click', () => importFile.click());
    importFile.addEventListener('change', async () => {
      const file = importFile.files[0];
      importFile.value = '';
      if (!file || !window.confirm('Replace all messages with this import?')) return;
      adminStatus.textContent = 'Importing messages ...';
      showOperationDialog('Importing messages', 'The board is validating and installing the imported store.');
      try {
        let totalMessages = 0;
        try {
          const archive = JSON.parse(await file.text());
          totalMessages = Array.isArray(archive.messages) ? archive.messages.length : 0;
        } catch (error) {
        }
        if (totalMessages > 0) {
          operationDialogMessage.textContent = `Importing messages: 0 / ${totalMessages}`;
        }
        let response;
        try {
          response = await uploadImportFile(file, totalMessages);
        } catch (error) {
          setAdminError('Import may have completed, but the connection was lost.');
          return;
        }
        if (response.status === 401) {
          setAdminState(false, 'Your admin session has expired.');
          return;
        }
        if (response.status < 200 || response.status >= 300) {
          const responseText = response.responseText;
          let reason = responseText;
          try {
            const error = JSON.parse(responseText);
            reason = error.reason || error.error || responseText;
          } catch (error) {
          }
          const detail = reason ? ` (${reason})` : '';
          setAdminError(`Import failed${detail}.`);
          return;
        }
        adminStatus.textContent = 'Messages imported.';
        try {
          await refreshMessages('full');
        } catch (error) {
          setAdminError('Messages imported, but the board display could not be refreshed.');
        }
      } catch (error) {
        setAdminError('Import status could not be determined.');
      } finally {
        hideOperationDialog();
      }
    });
    deviceDateTime.value = formatLocalDateTime(new Date());

    clockForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const selectedTime = new Date(deviceDateTime.value);
      if (Number.isNaN(selectedTime.getTime())) {
        clockStatus.textContent = 'Enter a valid local date and time.';
        return;
      }

      const body = new URLSearchParams({ epoch: String(Math.floor(selectedTime.getTime() / 1000)) });
      try {
        const response = await fetch('/api/time', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body
        });
        if (response.status === 401) {
          setAdminState(false, 'Your admin session has expired.');
          return;
        }
        if (!response.ok) {
          const data = await response.json().catch(() => ({}));
          throw new Error(data.error === 'clock_busy'
            ? 'The device clock is busy. Try again in a moment.'
            : 'Could not set device time.');
        }
        await refreshMessages();
      } catch (error) {
        clockStatus.textContent = `${error.message} Check the connection and try again.`;
      }
    });

    refreshAdminState();
    refreshMessages().then(scheduleNextPoll);
    setInterval(renderHeaderClock, 1000);
  </script>
</body>
</html>
)AREDNHTML";