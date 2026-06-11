#include "AdminHttpServer.hpp"

#include "httplib.h"

#include <atomic>
#include <thread>

namespace
{
    const std::string sAdminBrowserHtml = std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenMW MP Database Browser</title>
  <style>
    :root {
      --bg: #11110f;
      --panel: #1a1713;
      --panel-2: #221d17;
      --line: #8c7550;
      --text: #e1c98a;
      --muted: #b49a63;
      --accent: #d7b76f;
      --accent-2: #6c5331;
      --danger: #c78062;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Georgia, "Times New Roman", serif;
      background:
        radial-gradient(circle at top left, rgba(125, 94, 44, 0.16), transparent 34%),
        linear-gradient(180deg, #14120f 0%, var(--bg) 100%);
      color: var(--text);
    }
    .shell {
      max-width: 1500px;
      margin: 0 auto;
      padding: 20px;
    }
    .hero {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      gap: 10px 18px;
      align-items: start;
      margin-bottom: 18px;
      padding: 18px 20px;
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(63, 49, 31, 0.6), rgba(20, 18, 15, 0.92));
      box-shadow: inset 0 0 0 1px rgba(255, 232, 176, 0.05);
    }
    .hero-main {
      display: grid;
      gap: 10px;
      min-width: 0;
    }
    .hero-actions {
      display: flex;
      gap: 10px;
      align-items: center;
      justify-content: flex-end;
    }
    h1 {
      margin: 0;
      font-size: 30px;
      letter-spacing: 0.03em;
      font-weight: 600;
    }
    .sub {
      color: var(--muted);
      font-size: 15px;
    }
    .status {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 2px;
      font-size: 14px;
    }
    .badge {
      padding: 4px 9px;
      border: 1px solid var(--line);
      background: rgba(35, 30, 24, 0.8);
      color: var(--text);
    }
    .grid {
      display: grid;
      grid-template-columns: 300px minmax(0, 1fr);
      gap: 18px;
      align-items: start;
    }
    .panel {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(30, 26, 21, 0.96), rgba(18, 16, 13, 0.96));
      min-height: 0;
    }
    .panel-head {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      padding: 12px 14px;
      border-bottom: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(91, 69, 39, 0.42), rgba(32, 26, 20, 0.3));
    }
    .panel-head h2 {
      margin: 0;
      font-size: 18px;
      font-weight: 600;
    }
    .table-list {
      max-height: calc(100vh - 220px);
      overflow: auto;
    }
    .table-item {
      width: 100%;
      border: 0;
      border-bottom: 1px solid rgba(140, 117, 80, 0.22);
      padding: 11px 14px;
      text-align: left;
      background: transparent;
      color: inherit;
      cursor: pointer;
      font: inherit;
    }
    .table-item:hover,
    .table-item.active {
      background: rgba(119, 90, 49, 0.18);
    }
    .table-item small {
      display: block;
      margin-top: 2px;
      color: var(--muted);
    }
    .toolbar {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      align-items: center;
      padding: 12px 14px;
      border-bottom: 1px solid var(--line);
      background: rgba(27, 23, 19, 0.94);
    }
    button,
    .button-link,
    select,
    input {
      font: inherit;
    }
    button,
    .button-link {
      padding: 7px 12px;
      border: 1px solid var(--line);
      background: linear-gradient(180deg, #4b3922, #2c2218);
      color: var(--text);
      cursor: pointer;
      text-decoration: none;
      display: inline-block;
    }
    .cell-link {
      padding: 0 3px;
      border: 1px solid rgba(215, 183, 111, 0.42);
      background: rgba(76, 57, 34, 0.28);
      color: #ffe7a3;
      text-decoration: underline;
      text-underline-offset: 2px;
      cursor: pointer;
    }
    button:disabled {
      opacity: 0.45;
      cursor: default;
    }
    select,
    input {
      padding: 7px 9px;
      border: 1px solid var(--line);
      background: var(--panel-2);
      color: var(--text);
      min-width: 84px;
    }
    .toolbar .spacer {
      flex: 1 1 auto;
    }
    .table-wrap {
      overflow: auto;
      max-height: calc(100vh - 290px);
    }
    table {
      width: max-content;
      min-width: 100%;
      border-collapse: collapse;
    }
    thead th {
      position: sticky;
      top: 0;
      z-index: 1;
      background: #241d16;
      color: var(--accent);
    }
    th,
    td {
      padding: 8px 10px;
      border-bottom: 1px solid rgba(140, 117, 80, 0.22);
      border-right: 1px solid rgba(140, 117, 80, 0.12);
      vertical-align: top;
      text-align: left;
      white-space: nowrap;
      word-break: normal;
    }
    tbody tr:nth-child(odd) {
      background: rgba(255, 255, 255, 0.015);
    }
    .meta {
      padding: 10px 14px 14px;
      color: var(--muted);
      font-size: 14px;
    }
    .empty,
    .error {
      padding: 18px 14px;
      color: var(--muted);
    }
    .error { color: var(--danger); }
    .help-grid {
      display: grid;
      grid-template-columns: minmax(0, 1fr) minmax(280px, 420px);
      gap: 18px;
      align-items: start;
    }
    .help-content {
      display: grid;
      gap: 18px;
    }
    .help-card {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(30, 26, 21, 0.96), rgba(18, 16, 13, 0.96));
      padding: 18px 20px;
    }
    .help-card h2,
    .help-card h3 {
      margin: 0 0 10px;
      color: var(--accent);
      letter-spacing: 0.02em;
    }
    .help-card p,
    .help-card li {
      line-height: 1.45;
      color: var(--text);
    }
    .help-card p {
      margin: 0 0 12px;
    }
    .help-card ul,
    .help-card ol {
      margin: 0;
      padding-left: 22px;
    }
    .help-card li + li {
      margin-top: 8px;
    }
    .help-note {
      border-left: 3px solid var(--accent);
      padding: 10px 12px;
      background: rgba(119, 90, 49, 0.16);
      color: var(--muted);
    }
    code {
      color: #ffe7a3;
      background: rgba(0, 0, 0, 0.24);
      padding: 1px 4px;
      border: 1px solid rgba(140, 117, 80, 0.25);
    }
    .schema-table {
      width: 100%;
      min-width: 0;
      border-collapse: collapse;
    }
    .schema-table th,
    .schema-table td {
      white-space: normal;
    }
    @media (max-width: 960px) {
      .hero,
      .grid {
        grid-template-columns: 1fr;
      }
      .hero-actions {
        justify-content: flex-start;
      }
      .help-grid {
        grid-template-columns: 1fr;
      }
      .table-list,
      .table-wrap {
        max-height: none;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <div class="hero-main">
        <h1>OpenMW MP Database Browser</h1>
        <div class="sub">Loopback-only read-only browser over the approved server-side table/page API.</div>
        <div class="status" id="statusBar">
          <span class="badge">Connecting...</span>
        </div>
      </div>
      <div class="hero-actions">
        <a class="button-link" href="/admin/help">Help</a>
      </div>
    </section>

    <section class="grid">
      <aside class="panel">
        <div class="panel-head">
          <h2>Tables</h2>
          <button id="reloadTables">Reload</button>
        </div>
        <div class="table-list" id="tableList"></div>
      </aside>

      <main class="panel">
        <div class="panel-head">
          <h2 id="pageTitle">Rows</h2>
          <span id="pageSummary" class="sub"></span>
        </div>
        <div class="toolbar">
          <label>Limit
            <select id="pageLimit">
              <option value="25">25</option>
              <option value="50">50</option>
              <option value="100" selected>100</option>
              <option value="250">250</option>
            </select>
          </label>
          <button id="prevPage">Prev</button>
          <button id="nextPage">Next</button>
          <button id="reloadPage">Reload</button>
          <div class="spacer"></div>
          <input id="rowFilter" type="search" placeholder="Filter visible rows">
        </div>
        <div class="table-wrap" id="tableWrap">
          <div class="empty" id="emptyState">Select a table to load rows.</div>
          <table id="dataTable" hidden>
            <thead><tr id="tableHead"></tr></thead>
            <tbody id="tableBody"></tbody>
          </table>
        </div>
        <div class="meta" id="metaLine"></div>
      </main>
    </section>
  </div>

  <script>
)HTML")
        + R"HTML(
    const state = {
      tables: [],
      selectedTable: null,
      offset: 0,
      limit: 100,
      page: null,
      pendingFilter: '',
      accountsById: new Map(),
    };

    const statusBar = document.getElementById('statusBar');
    const tableList = document.getElementById('tableList');
    const pageTitle = document.getElementById('pageTitle');
    const pageSummary = document.getElementById('pageSummary');
    const pageLimit = document.getElementById('pageLimit');
    const prevPage = document.getElementById('prevPage');
    const nextPage = document.getElementById('nextPage');
    const reloadTables = document.getElementById('reloadTables');
    const reloadPage = document.getElementById('reloadPage');
    const rowFilter = document.getElementById('rowFilter');
    const emptyState = document.getElementById('emptyState');
    const dataTable = document.getElementById('dataTable');
    const tableHead = document.getElementById('tableHead');
    const tableBody = document.getElementById('tableBody');
    const metaLine = document.getElementById('metaLine');

    async function fetchJson(url) {
      const response = await fetch(url, { cache: 'no-store' });
      const text = await response.text();
      let data = null;
      try {
        data = text ? JSON.parse(text) : null;
      } catch (error) {
        throw new Error(`Invalid JSON from ${url}`);
      }
      if (!response.ok || (data && data.ok === false)) {
        throw new Error((data && data.error) || `HTTP ${response.status}`);
      }
      return data;
    }

    function setStatus(parts) {
      statusBar.innerHTML = '';
      parts.forEach((text) => {
        const span = document.createElement('span');
        span.className = 'badge';
        span.textContent = text;
        statusBar.appendChild(span);
      });
    }

    function renderTables() {
      tableList.innerHTML = '';
      if (!state.tables.length) {
        tableList.innerHTML = '<div class="empty">No approved tables are available.</div>';
        return;
      }

      state.tables.forEach((entry) => {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'table-item' + (entry.name === state.selectedTable ? ' active' : '');
        button.innerHTML = `<strong>${entry.name}</strong><small>${entry.rowCount} rows</small>`;
        button.addEventListener('click', () => {
          if (state.selectedTable === entry.name) return;
          state.selectedTable = entry.name;
          state.offset = 0;
          renderTables();
          loadPage();
        });
        tableList.appendChild(button);
      });
    }

    function tableExists(tableName) {
      return state.tables.some((entry) => entry.name === tableName);
    }

    function accountNameForId(accountId) {
      const key = String(accountId ?? '').trim();
      return key ? (state.accountsById.get(key) || '') : '';
    }

    function displayValueForCell(row, column, value) {
      const text = String(value ?? '');
      if (state.selectedTable === 'characters' && column === 'account_id') {
        return accountNameForId(value) || text;
      }
      return text;
    }

    function searchValueForCell(row, column, value) {
      const text = String(value ?? '');
      const display = displayValueForCell(row, column, value);
      return display === text ? text : `${display} ${text}`;
    }

    function headerLabelForColumn(column) {
      if (state.selectedTable === 'characters' && column === 'account_id') {
        return 'account';
      }
      return column;
    }

    function goToTable(tableName, filter) {
      if (!tableExists(tableName)) return;
      state.selectedTable = tableName;
      state.offset = 0;
      state.pendingFilter = filter || '';
      renderTables();
      loadPage();
    }

    function dynamicRecordFilter(recordId) {
      return String(recordId || '').trim();
    }

    function ownerFilter(row) {
      return [row.owner_a, row.owner_b, row.owner_c, row.owner_index]
        .filter((value) => value !== undefined && value !== null && String(value) !== '')
        .join(' ');
    }

)HTML"
        + R"HTML(
    function linkTargetForCell(row, column, value) {
      const table = state.selectedTable;
      const text = String(value ?? '').trim();
      if (!text) return null;

      if (table === 'characters' && column === 'id') {
        return { table: 'character_inventory', filter: text, title: 'View saved inventory for this character' };
      }
      if (table === 'characters' && column === 'account_id') {
        return { table: 'accounts', filter: accountNameForId(text) || text, title: 'View owning account' };
      }
      if (table === 'accounts' && column === 'id') {
        return { table: 'characters', filter: accountNameForId(text) || text, title: 'View characters using this account' };
      }

      if ((table === 'character_inventory' || table === 'character_equipment' || table === 'character_marks')
          && column === 'character_id') {
        return { table: 'characters', filter: text, title: 'View character row' };
      }

      if ((column === 'record_id' || column === 'ref_id') && text.startsWith('$')) {
        return { table: 'world_dynamic_record_catalog', filter: dynamicRecordFilter(text), title: 'View dynamic record catalog entry' };
      }

      if (table === 'world_dynamic_record_catalog' && column === 'record_id') {
        return { table: 'world_dynamic_record_links', filter: dynamicRecordFilter(text), title: 'View links keeping this record alive' };
      }
      if (table === 'world_dynamic_records' && column === 'record_id') {
        return { table: 'world_dynamic_record_catalog', filter: dynamicRecordFilter(text), title: 'View catalog metadata for this record' };
      }

      if (table === 'world_dynamic_record_links') {
        if (column === 'record_id') {
          return { table: 'world_dynamic_record_catalog', filter: dynamicRecordFilter(text), title: 'View linked dynamic record' };
        }

        const kind = row.link_kind || '';
        if (kind === 'inventory_item') {
          if (column === 'owner_a') return { table: 'characters', filter: text, title: 'View owning character' };
          if (column === 'owner_index') return { table: 'character_inventory', filter: `${row.owner_a} ${text}`, title: 'View inventory slot row' };
        }
        if (kind === 'equipment_item') {
          if (column === 'owner_a') return { table: 'characters', filter: text, title: 'View owning character' };
          if (column === 'owner_index') return { table: 'character_equipment', filter: `${row.owner_a} ${text}`, title: 'View equipment slot row' };
        }
        if (kind === 'placed_object' && (column === 'owner_a' || column === 'owner_b')) {
          return { table: 'world_objects', filter: ownerFilter(row), title: 'View placed object owner' };
        }
        if ((kind === 'container_parent' || kind === 'container_item')
            && (column === 'owner_a' || column === 'owner_b' || column === 'owner_c')) {
          return { table: 'world_containers', filter: ownerFilter(row), title: 'View owning container' };
        }
        if (kind === 'container_item' && column === 'owner_index') {
          return { table: 'world_container_items', filter: ownerFilter(row), title: 'View container item row' };
        }
        if (kind === 'door_state' && (column === 'owner_a' || column === 'owner_b' || column === 'owner_c')) {
          return { table: 'world_doors', filter: ownerFilter(row), title: 'View door state owner' };
        }
        if (kind === 'record_dependency' && (column === 'owner_a' || column === 'owner_b')) {
          return { table: 'world_dynamic_record_catalog', filter: row.owner_b || row.owner_a, title: 'View owning dynamic record' };
        }
      }

      return null;
    }

    function appendCellValue(td, row, column, value) {
      const displayValue = displayValueForCell(row, column, value);
      const target = linkTargetForCell(row, column, value);
      if (!target || !tableExists(target.table)) {
        td.textContent = displayValue;
        return;
      }

      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'cell-link';
      button.textContent = displayValue;
      button.title = target.title;
      button.addEventListener('click', (event) => {
        event.stopPropagation();
        goToTable(target.table, target.filter);
      });
      td.appendChild(button);
    }

    function renderPage() {
      const page = state.page;
      tableHead.innerHTML = '';
      tableBody.innerHTML = '';

      if (!page || !page.columns || !page.columns.length) {
        dataTable.hidden = true;
        emptyState.hidden = false;
        emptyState.textContent = state.selectedTable ? 'No rows in the selected page.' : 'Select a table to load rows.';
        pageTitle.textContent = state.selectedTable || 'Rows';
        pageSummary.textContent = '';
        metaLine.textContent = '';
        prevPage.disabled = true;
        nextPage.disabled = true;
        return;
      }

      pageTitle.textContent = page.tableName;
      const start = page.rows.length ? page.offset + 1 : 0;
      const end = page.offset + page.rows.length;
      pageSummary.textContent = `rows ${start}-${end} of ${page.totalRows}`;
      metaLine.textContent = `${page.columns.length} columns | offset ${page.offset} | limit ${page.limit}`;

      page.columns.forEach((column) => {
        const th = document.createElement('th');
        th.textContent = headerLabelForColumn(column);
        tableHead.appendChild(th);
      });

      const filter = rowFilter.value.trim().toLowerCase();
      let visibleRows = 0;
      page.rows.forEach((row) => {
        const values = page.columns.map((column) => searchValueForCell(row, column, row[column] ?? ''));
        const joined = values.join(' ').toLowerCase();
        if (filter && !joined.includes(filter)) return;

        const tr = document.createElement('tr');
        page.columns.forEach((column) => {
          const value = row[column] ?? '';
          const td = document.createElement('td');
          appendCellValue(td, row, column, value);
          tr.appendChild(td);
        });
        tableBody.appendChild(tr);
        visibleRows += 1;
      });

      emptyState.hidden = true;
      dataTable.hidden = false;
      if (!visibleRows) {
        dataTable.hidden = true;
        emptyState.hidden = false;
        emptyState.textContent = 'No visible rows match the current filter.';
      }

      prevPage.disabled = page.offset <= 0;
      nextPage.disabled = (page.offset + page.rows.length) >= page.totalRows;
    }

    async function loadHealth() {
      const data = await fetchJson('/api/admin/health');
      setStatus([
        `Players ${data.playerCount}`,
        `Time ${Number(data.worldHour).toFixed(2)}`,
        `Uptime ${Math.floor(data.uptime)}s`,
        `Tables ${data.tableCount}`,
      ]);
    }

    async function loadTables() {
      const data = await fetchJson('/api/admin/database/tables');
      state.tables = data.tables || [];
      if (!state.selectedTable || !state.tables.find((entry) => entry.name === state.selectedTable)) {
        state.selectedTable = state.tables[0] ? state.tables[0].name : null;
        state.offset = 0;
      }
      renderTables();
    }

    async function loadAccountLookup() {
      state.accountsById = new Map();
      if (!tableExists('accounts')) return;

      const params = new URLSearchParams({
        table: 'accounts',
        offset: '0',
        limit: '500',
      });
      const data = await fetchJson(`/api/admin/database/page?${params.toString()}`);
      const rows = (data.page && data.page.rows) || [];
      rows.forEach((row) => {
        if (row.id === undefined || row.id === null || !row.username) return;
        state.accountsById.set(String(row.id), String(row.username));
      });
    }

    async function loadPage() {
      if (!state.selectedTable) {
        state.page = null;
        renderPage();
        return;
      }

      emptyState.hidden = false;
      emptyState.textContent = 'Loading rows...';
      dataTable.hidden = true;

      const params = new URLSearchParams({
        table: state.selectedTable,
        offset: String(state.offset),
        limit: String(state.limit),
      });
      const data = await fetchJson(`/api/admin/database/page?${params.toString()}`);
      state.page = data.page || null;
      if (state.pendingFilter) {
        rowFilter.value = state.pendingFilter;
        state.pendingFilter = '';
      }
      renderPage();
    }

    async function reloadAll() {
      try {
        await loadHealth();
        await loadTables();
        await loadAccountLookup();
        await loadPage();
      } catch (error) {
        setStatus([`Error: ${error.message}`]);
        emptyState.hidden = false;
        emptyState.textContent = error.message;
        dataTable.hidden = true;
      }
    }

    pageLimit.addEventListener('change', () => {
      state.limit = Number(pageLimit.value) || 100;
      state.offset = 0;
      loadPage();
    });
    prevPage.addEventListener('click', () => {
      state.offset = Math.max(0, state.offset - state.limit);
      loadPage();
    });
    nextPage.addEventListener('click', () => {
      state.offset += state.limit;
      loadPage();
    });
    reloadTables.addEventListener('click', reloadAll);
    reloadPage.addEventListener('click', loadPage);
    rowFilter.addEventListener('input', renderPage);

    reloadAll();
    setInterval(loadHealth, 5000);
  </script>
</body>
</html>)HTML";

    constexpr const char* sAdminHelpHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenMW MP Database Help</title>
  <style>
    :root {
      --bg: #11110f;
      --panel: #1a1713;
      --panel-2: #221d17;
      --line: #8c7550;
      --text: #e1c98a;
      --muted: #b49a63;
      --accent: #d7b76f;
      --danger: #c78062;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Georgia, "Times New Roman", serif;
      background:
        radial-gradient(circle at top left, rgba(125, 94, 44, 0.18), transparent 34%),
        linear-gradient(180deg, #14120f 0%, var(--bg) 100%);
      color: var(--text);
    }
    .shell {
      max-width: 1500px;
      margin: 0 auto;
      padding: 20px;
    }
    .hero {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      gap: 10px 18px;
      align-items: start;
      margin-bottom: 18px;
      padding: 18px 20px;
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(63, 49, 31, 0.6), rgba(20, 18, 15, 0.92));
      box-shadow: inset 0 0 0 1px rgba(255, 232, 176, 0.05);
    }
    .hero-main {
      display: grid;
      gap: 10px;
      min-width: 0;
    }
    h1 {
      margin: 0;
      font-size: 30px;
      letter-spacing: 0.03em;
      font-weight: 600;
    }
    .sub {
      color: var(--muted);
      font-size: 15px;
    }
    .button-link {
      padding: 7px 12px;
      border: 1px solid var(--line);
      background: linear-gradient(180deg, #4b3922, #2c2218);
      color: var(--text);
      cursor: pointer;
      text-decoration: none;
      display: inline-block;
    }
    .help-grid {
      display: grid;
      grid-template-columns: minmax(0, 1fr) minmax(280px, 420px);
      gap: 18px;
      align-items: start;
    }
    .help-content {
      display: grid;
      gap: 18px;
    }
    .help-card {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(30, 26, 21, 0.96), rgba(18, 16, 13, 0.96));
      padding: 18px 20px;
    }
    .help-card h2,
    .help-card h3 {
      margin: 0 0 10px;
      color: var(--accent);
      letter-spacing: 0.02em;
    }
    .help-card p,
    .help-card li {
      line-height: 1.45;
      color: var(--text);
    }
    .help-card p {
      margin: 0 0 12px;
    }
    .help-card ul,
    .help-card ol {
      margin: 0;
      padding-left: 22px;
    }
    .help-card li + li {
      margin-top: 8px;
    }
    .help-note {
      border-left: 3px solid var(--accent);
      padding: 10px 12px;
      background: rgba(119, 90, 49, 0.16);
      color: var(--muted);
    }
    code {
      color: #ffe7a3;
      background: rgba(0, 0, 0, 0.24);
      padding: 1px 4px;
      border: 1px solid rgba(140, 117, 80, 0.25);
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 10px;
    }
    th,
    td {
      padding: 8px 10px;
      border-bottom: 1px solid rgba(140, 117, 80, 0.22);
      border-right: 1px solid rgba(140, 117, 80, 0.12);
      vertical-align: top;
      text-align: left;
    }
    th {
      color: var(--accent);
      background: #241d16;
    }
    @media (max-width: 960px) {
      .hero,
      .help-grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <div class="hero-main">
        <h1>Database Browser Help</h1>
        <div class="sub">How the server database fits into multiplayer persistence, dynamic records, and future live editing.</div>
      </div>
      <a class="button-link" href="/admin/">Back to Browser</a>
    </section>

    <section class="help-grid">
      <main class="help-content">
        <article class="help-card">
          <h2>What This Database Is</h2>
          <p>The multiplayer server uses one SQLite database, normally <code>playerdata.db</code>, as its authoritative persistence store. It is not a copy of Morrowind.esm and it is not a client save file. It stores the multiplayer state that must survive server restarts.</p>
          <p>The database browser is a loopback-only, read-only view over approved tables. It exists so server operators can inspect what the server thinks is true without opening SQLite manually or issuing arbitrary SQL.</p>
          <div class="help-note">Current safety model: this page can browse approved tables, but it cannot write rows yet. That is intentional until live-edit validation and audit behavior are implemented.</div>
        </article>

        <article class="help-card">
          <h2>What It Stores</h2>
          <table>
            <thead>
              <tr><th>Area</th><th>Tables</th><th>Purpose</th></tr>
            </thead>
            <tbody>
              <tr>
                <td>Accounts and characters</td>
                <td><code>accounts</code>, <code>characters</code>, <code>account_keypairs</code></td>
                <td>Login identity, character slots, chargen data, nickname, saved position, and key-pair auth data.</td>
              </tr>
              <tr>
                <td>Player state</td>
                <td><code>character_inventory</code>, <code>character_equipment</code>, <code>character_marks</code></td>
                <td>Saved inventory snapshots, equipped items, and mark/recall locations for each character.</td>
              </tr>
              <tr>
                <td>World state</td>
                <td><code>world_objects</code>, <code>world_containers</code>, <code>world_container_items</code>, <code>world_doors</code></td>
                <td>Server-placed objects, authoritative container contents, and door states that persist across restarts.</td>
              </tr>
              <tr>
                <td>Dynamic records</td>
                <td><code>world_dynamic_records</code>, <code>world_dynamic_record_catalog</code>, <code>world_dynamic_record_links</code></td>
                <td>Custom records created by the server, lifetime metadata, and the persisted references that keep generated records alive.</td>
              </tr>
            </tbody>
          </table>
        </article>

        <article class="help-card">
          <h2>How It Works Technically</h2>
          <ul>
            <li>The server owns the database through <code>PlayerDatabase</code>. Database calls are synchronous and must be made from the server/Lua-owned control path, not directly from arbitrary browser code.</li>
            <li>The web browser calls loopback HTTP endpoints in <code>openmw-server</code>. Those endpoints call the same Lua admin service used by <code>/helpmenu</code>.</li>
            <li>The browser never gets raw SQL access. It asks for the approved table catalog or one paged slice of one approved table.</li>
            <li>Dynamic records are stored separately from the things that reference them. The link table is how the server knows whether a generated record is still used by placed objects, containers, inventory, equipment, or other dynamic records.</li>
            <li>Session-only dynamic records are cleaned up on restart. Persistent dynamic records are replayed to clients before relevant world state so custom placed objects and inventory refs can resolve.</li>
          </ul>
        </article>

        <article class="help-card">
          <h2>How To Read The Tables</h2>
          <ul>
            <li>Use <code>characters</code> to find a character id, then use that id in <code>character_inventory</code>, <code>character_equipment</code>, and <code>character_marks</code>.</li>
            <li>Use <code>world_dynamic_record_catalog</code> to see whether a custom record is generated or permanent, persistent or session-only, and how many persisted links point at it.</li>
            <li>Use <code>world_dynamic_record_links</code> to understand why a generated record is still alive. A weapon in player inventory, a placed object, and a dependency from another dynamic record all appear as links.</li>
            <li>Use <code>world_dynamic_records</code> when you need the serialized payload for persistent custom records. The payload is server-authored Lua data serialized by the dynamic-record system.</li>
          </ul>
        </article>

        <article class="help-card">
          <h2>Dynamic Link Owner Columns</h2>
          <p>The <code>world_dynamic_record_links</code> table is how the server explains why a dynamic record is still considered referenced. The <code>record_id</code> column is the dynamic record being kept alive. The <code>link_kind</code> column tells you what kind of thing owns that reference. The <code>owner_a</code>, <code>owner_b</code>, <code>owner_c</code>, and <code>owner_index</code> columns are generic owner keys whose meaning changes by <code>link_kind</code>.</p>
          <table>
            <thead>
              <tr>
                <th>link_kind</th>
                <th>owner_a</th>
                <th>owner_b</th>
                <th>owner_c</th>
                <th>owner_index</th>
              </tr>
            </thead>
            <tbody>
              <tr>
                <td><code>inventory_item</code></td>
                <td>Character id</td>
                <td>Empty</td>
                <td>Empty</td>
                <td>Inventory item index in that saved snapshot</td>
              </tr>
              <tr>
                <td><code>equipment_item</code></td>
                <td>Character id</td>
                <td>Empty</td>
                <td>Empty</td>
                <td>Equipment slot number</td>
              </tr>
              <tr>
                <td><code>placed_object</code></td>
                <td>Placed object's <code>mpNum</code></td>
                <td>Cell id</td>
                <td>Empty</td>
                <td>Currently unused, normally <code>0</code></td>
              </tr>
              <tr>
                <td><code>spawned_actor</code></td>
                <td>Spawned actor's server <code>mpNum</code></td>
                <td>Cell id</td>
                <td>Empty</td>
                <td>Currently unused, normally <code>0</code></td>
              </tr>
              <tr>
                <td><code>container_parent</code></td>
                <td>Cell id</td>
                <td>Container ref id</td>
                <td>Container refNum</td>
                <td>Currently unused, normally <code>0</code></td>
              </tr>
              <tr>
                <td><code>container_item</code></td>
                <td>Cell id</td>
                <td>Container ref id</td>
                <td>Container refNum</td>
                <td>Item index inside the authoritative container snapshot</td>
              </tr>
              <tr>
                <td><code>door_state</code></td>
                <td>Cell id</td>
                <td>Door ref id</td>
                <td>Door refNum</td>
                <td>Currently unused, normally <code>0</code></td>
              </tr>
              <tr>
                <td><code>record_dependency</code></td>
                <td>Owning dynamic record type</td>
                <td>Owning dynamic record id</td>
                <td>Empty</td>
                <td>Dependency index in the owning record's dependency list</td>
              </tr>
            </tbody>
          </table>
          <p>Example: if <code>record_id=$custom_weapon_4</code>, <code>link_kind=inventory_item</code>, and <code>owner_a=5</code>, then character id <code>5</code> has that dynamic weapon in saved inventory. If <code>link_kind=record_dependency</code>, then another dynamic record depends on <code>$custom_weapon_4</code>; use <code>owner_a</code> and <code>owner_b</code> to find that owning record.</p>
        </article>

        <article class="help-card">
          <h2>Planned Live Editing Workflow</h2>
          <p>Live editing should not become arbitrary SQL in a browser. The safe version is an action-based workflow where each edit routes through server APIs that already know how to validate, broadcast, and persist changes.</p>
          <ol>
            <li>Select a row in the browser.</li>
            <li>The browser maps that row to a typed admin action, such as edit character nickname, grant inventory item, remove a placed object, update a dynamic record payload, or clean dangling links.</li>
            <li>The server validates permissions, table/action compatibility, expected row version, and payload shape.</li>
            <li>The server applies the change through the existing authoritative path, not by directly mutating SQLite behind the runtime state.</li>
            <li>The server broadcasts any required packets or Lua events so connected clients stay synchronized.</li>
            <li>The browser refreshes the affected table and shows an audit-style result message.</li>
          </ol>
          <div class="help-note">The future rule should be: inspect tables freely, but mutate through explicit server actions only.</div>
        </article>
      </main>

      <aside class="help-content">
        <article class="help-card">
          <h3>Current Endpoints</h3>
          <ul>
            <li><code>/admin/</code> opens the database browser.</li>
            <li><code>/admin/help</code> opens this help page.</li>
            <li><code>/api/admin/health</code> returns player count, world time, uptime, and table count.</li>
            <li><code>/api/admin/database/tables</code> returns approved table names and row counts.</li>
            <li><code>/api/admin/database/page?table=...&amp;offset=...&amp;limit=...</code> returns one row page.</li>
          </ul>
        </article>

        <article class="help-card">
          <h3>Safety Boundaries</h3>
          <ul>
            <li>The listener is intended for <code>127.0.0.1</code> only.</li>
            <li>The server currently forces non-loopback host config back to loopback.</li>
            <li>No write endpoints exist yet.</li>
            <li>No direct SQL endpoint should be added for normal admin use.</li>
          </ul>
        </article>

        <article class="help-card">
          <h3>Future Edit Examples</h3>
          <ul>
            <li>Grant or remove a player inventory item through the same authoritative inventory path used by server scripts.</li>
            <li>Patch a dynamic record, then broadcast <code>RecordDynamic</code> so connected clients update immediately.</li>
            <li>Delete a placed object and remove the matching dynamic-record link so generated-record GC can run safely.</li>
            <li>Repair dangling references by using the same cleanup routines that restart cleanup already uses.</li>
          </ul>
        </article>
      </aside>
    </section>
  </div>
</body>
</html>)HTML";

    std::map<std::string, std::string> copyQueryParams(const httplib::Request& req)
    {
        std::map<std::string, std::string> result;
        for (const auto& [key, value] : req.params)
            result[key] = value;
        return result;
    }

    void applyResponse(const mwmp::AdminHttpServer::Response& response, httplib::Response& res)
    {
        res.status = response.status > 0 ? response.status : 200;
        res.set_content(response.body, response.contentType.c_str());
    }
}

namespace mwmp
{

struct AdminHttpServer::Impl
{
    Handler handler;
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    std::atomic<bool> running { false };
    std::string host = "127.0.0.1";
    int port = 0;
};

AdminHttpServer::AdminHttpServer(Handler handler)
    : mImpl(std::make_unique<Impl>())
{
    mImpl->handler = std::move(handler);
}

AdminHttpServer::~AdminHttpServer()
{
    stop();
}

bool AdminHttpServer::start(const std::string& host, int port, std::string* error)
{
    if (host.empty() || port <= 0)
    {
        if (error)
            *error = "invalid_host_or_port";
        return false;
    }

    stop();

    mImpl->host = host;
    mImpl->port = port;
    mImpl->server = std::make_unique<httplib::Server>();

    auto& server = *mImpl->server;
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/admin/");
    });
    server.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/admin/");
    });
    server.Get("/admin/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(sAdminBrowserHtml, "text/html; charset=utf-8");
    });
    server.Get("/admin/help", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(sAdminHelpHtml, "text/html; charset=utf-8");
    });

    server.Get("/api/admin/health", [this](const httplib::Request& req, httplib::Response& res) {
        applyResponse(mImpl->handler("health", copyQueryParams(req)), res);
    });
    server.Get("/api/admin/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        applyResponse(mImpl->handler("snapshot", copyQueryParams(req)), res);
    });
    server.Get("/api/admin/database/tables", [this](const httplib::Request& req, httplib::Response& res) {
        applyResponse(mImpl->handler("database_tables", copyQueryParams(req)), res);
    });
    server.Get("/api/admin/database/page", [this](const httplib::Request& req, httplib::Response& res) {
        applyResponse(mImpl->handler("database_browse", copyQueryParams(req)), res);
    });
    server.Post("/api/admin/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        applyResponse(mImpl->handler("shutdown", copyQueryParams(req)), res);
    });

    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "internal_server_error";
        try
        {
            if (ep)
                std::rethrow_exception(ep);
        }
        catch (const std::exception& e)
        {
            message = e.what();
        }
        res.status = 500;
        res.set_content("{\"ok\":false,\"error\":\"" + message + "\"}", "application/json; charset=utf-8");
    });

    if (!server.bind_to_port(host, port))
    {
        if (error)
            *error = "bind_failed";
        mImpl->server.reset();
        return false;
    }

    mImpl->thread = std::thread([this]() {
        mImpl->running = true;
        mImpl->server->listen_after_bind();
        mImpl->running = false;
    });
    server.wait_until_ready();
    return true;
}

void AdminHttpServer::stop()
{
    if (mImpl->server)
        mImpl->server->stop();
    if (mImpl->thread.joinable())
        mImpl->thread.join();
    mImpl->running = false;
    mImpl->server.reset();
}

bool AdminHttpServer::isRunning() const
{
    return mImpl && mImpl->running.load();
}

std::string AdminHttpServer::url() const
{
    if (!mImpl || mImpl->host.empty() || mImpl->port <= 0)
        return {};
    return "http://" + mImpl->host + ":" + std::to_string(mImpl->port) + "/admin/";
}

} // namespace mwmp
