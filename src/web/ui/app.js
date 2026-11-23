// reforce web control UI. Plain JS, no build step; served from the binary.
// Talks to /api/library (once), /api/events (SSE state pushes) and
// /api/command (JSON commands). The engine owns all state; this page only
// renders snapshots and posts commands.
'use strict';
(() => {
  const MAIN = ['flowfield', 'waveshape', 'colormap'];
  const EXTRA = ['union', 'particle', 'sprite', 'movie', 'object', 'dancer'];
  // Card key -> engine layer: the Movie sprites card drives the Sprite layer's movie half.
  const layerOf = (key) => (key === 'movie' ? 'sprite' : key);
  const TITLES = {
    flowfield: 'FlowField', waveshape: 'WaveShape', colormap: 'ColorMap',
    union: 'FlowField Union', particle: 'Particles', sprite: 'Image sprites', movie: 'Movie sprites',
    object: 'Objects', dancer: 'Dancers',
  };

  const $ = (sel, root = document) => root.querySelector(sel);
  // DOM writes only when the value changes: the live event runs at 20 Hz and every
  // text write would otherwise cost a style + layout pass.
  const setText = (el, t) => { if (el && el.textContent !== t) el.textContent = t; };
  // Progress bars and meters scale with a transform (no layout), see app.css.
  const setScale = (el, axis, f) => {
    if (!el) return;
    const v = `scale${axis}(${Math.max(0, Math.min(1, f || 0)).toFixed(3)})`;
    if (el.style.transform !== v) el.style.transform = v;
  };
  const token = new URLSearchParams(location.search).get('token') || '';
  const api = (path) => token ? `${path}${path.includes('?') ? '&' : '?'}token=${encodeURIComponent(token)}` : path;

  const store = {
    get(k) { try { return localStorage.getItem(k); } catch { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch { /* private mode */ } },
  };
  // A collapsible section that remembers, per browser, whether it was open.
  const rememberOpen = (el, key) => {
    if (!el) return;
    el.open = store.get(key) === '1';
    el.addEventListener('toggle', () => store.set(key, el.open ? '1' : '0'));
  };
  // Slider label formats.
  const pct = (x) => Math.round(x * 100) + '%';
  const fmtInt = (x) => String(Math.round(x));

  let library = { layers: [], themes: [] };
  let state = null;
  let stateAt = 0;
  const views = new Map();

  // ------------------------------------------------------------ helpers
  let toastTimer = 0;
  function toast(msg, kind = 'error') {
    const t = $('#toast');
    t.textContent = msg;
    t.className = `toast ${kind === 'info' ? 'info' : ''}`;
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
  }

  async function post(cmd) {
    try {
      const r = await fetch(api('/api/command'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cmd),
      });
      if (!r.ok) {
        let msg = `HTTP ${r.status}`;
        try { msg = (await r.json()).error || msg; } catch { /* not JSON */ }
        toast(msg);
      } else if (cmd.seconds !== undefined) {
        try {
          const j = await r.json();
          if (j.saveError) toast(`Applied, but not saved: ${j.saveError}`);
        } catch { /* not JSON */ }
      }
    } catch {
      toast('Not connected to reforce');
    }
  }

  const fmtSec = (s) => (s >= 100 || Number.isInteger(s) ? s.toFixed(0) : s.toFixed(1)) + ' s';
  // Spawn intervals: 0 = off; minutes from 2 min up.
  const fmtSpan = (s) => (s <= 0 ? 'off' : s >= 120 ? `${+(s / 60).toFixed(1)} min` : fmtSec(s));
  const libLayer = (key) => library.layers.find((l) => l.key === key) || { key, configs: [], categories: [], blendModes: [] };
  const stateLayer = (key) => state && state.layers ? state.layers.find((l) => l.key === key) : undefined;
  // ---- Favourites (reforce.Favorites): top bar buttons + stars in the lists.
  let favorites = [];
  let favSaving = false;
  const isFav = (layer, id) => favorites.some((f) => f.layer === layer && f.id === id);
  async function loadFavorites() {
    try {
      const r = await fetch(api('/api/favorites'));
      if (r.ok) {
        const j = await r.json();
        favorites = j.favorites || [];
        favSaving = !!j.canSave;
      }
    } catch { /* not connected */ }
    renderFavorites();
  }
  async function toggleFavorite(layer, id) {
    try {
      const r = await fetch(api('/api/favorites'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ layer, id, on: !isFav(layer, id) }),
      });
      const j = await r.json();
      if (!r.ok) { toast(j.error || `HTTP ${r.status}`); return; }
      favorites = j.favorites || [];
      if (!j.canSave) toast('Favourite kept for this run only (no settings file)');
    } catch { toast('Not connected to reforce'); return; }
    renderFavorites();
    for (const v of views.values()) if (layerOf(v.key) === layer && v.list) renderList(v);
  }
  function renderFavorites() {
    const box = $('#favButtons');
    if (!box) return;
    box.replaceChildren();
    for (const f of favorites) {
      if (f.layer === 'sprite' && isHidden(f.id)) continue;  // hidden: reforce would refuse it
      const lib = libLayer(f.layer);
      const cfg = lib.configs.find((c) => c.id === f.id);
      const name = (cfg ? cfg.name : f.id.slice(f.id.lastIndexOf('/') + 1)).trim();
      const kind = document.createElement('span');
      kind.className = 'kind';
      kind.textContent = f.layer === 'sprite' ? (cfg && cfg.movie ? 'Movie sprite' : 'Image sprite')
                                              : (TITLES[f.layer] || f.layer).replace(/s$/, '');
      // A favourite whose scene is gone (removed or renamed; its ☆ is no longer in any list): greyed out, with a ✕ to
      // remove it. Only once that layer's library has loaded, so nothing is flagged before the lists arrive.
      if (!cfg && lib.configs.length > 0) {
        const m = document.createElement('span');
        m.className = 'fav-missing';
        m.title = `${f.id} is no longer in the library`;
        const x = document.createElement('button');
        x.className = 'fav-remove';
        x.textContent = '✕';
        x.title = `Remove the favourite ${f.id}`;
        x.addEventListener('click', () => toggleFavorite(f.layer, f.id));
        m.append(document.createTextNode(name), kind, x);
        box.append(m);
        continue;
      }
      const b = document.createElement('button');
      b.append(document.createTextNode(name), kind);
      b.title = f.layer === 'sprite' ? `Start sprite ${f.id}` : `Switch ${TITLES[f.layer]} to ${f.id}`;
      b.addEventListener('click', () => post({ layer: f.layer, action: 'switch', config: f.id }));
      box.append(b);
    }
    if (!favorites.length) {
      const h = document.createElement('span');
      h.className = 'hint-empty';
      h.textContent = 'Star (☆) a scene in a list to add a button here';
      box.append(h);
    }
  }

  const isCurrent = (cfg, ls) => !!ls && !!ls.current && (cfg.id === ls.current || cfg.name === ls.current);

  // Throttle: fn runs at most every `ms`, with a trailing call.
  function throttle(fn, ms) {
    let last = 0, timer = 0, pending = null;
    return (...args) => {
      pending = args;
      const wait = last + ms - Date.now();
      if (wait <= 0) {
        last = Date.now();
        fn(...pending);
        pending = null;
      } else if (!timer) {
        timer = setTimeout(() => {
          timer = 0;
          last = Date.now();
          if (pending) fn(...pending);
          pending = null;
        }, wait);
      }
    };
  }

  // ---- Hidden sprites (reforce.Sprite.Hidden): never started, by the slideshow or by hand.
  // The Image sprites card's "Review images…" opens a grid of every image sprite to hide / show them.
  let hiddenSprites = new Set();  // trimmed, lower-case ids
  let hiddenCanSave = false;
  const hidKey = (id) => id.trim().toLowerCase();
  const isHidden = (id) => hiddenSprites.has(hidKey(id));
  function takeHidden(j) {
    hiddenSprites = new Set((j.hidden || []).map(hidKey));
    hiddenCanSave = !!j.canSave;
  }
  async function loadHidden() {
    try {
      const r = await fetch(api('/api/sprites/hidden'));
      if (r.ok) takeHidden(await r.json());
    } catch { /* not connected */ }
  }
  function hiddenChanged() {
    renderFavorites();
    for (const v of views.values()) if (layerOf(v.key) === 'sprite' && v.list) renderList(v);
    updateReviewButton();
    if ($('#review').open) renderReview(false);
  }
  async function setHidden(ids, on) {
    const before = new Set(hiddenSprites);
    for (const id of ids) on ? hiddenSprites.add(hidKey(id)) : hiddenSprites.delete(hidKey(id));
    hiddenChanged();  // at once; put back if reforce says no
    try {
      const r = await fetch(api('/api/sprites/hidden'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids, on }),
      });
      const j = await r.json();
      if (!r.ok) throw new Error(j.error || `HTTP ${r.status}`);
      takeHidden(j);
      if (!j.canSave) toast('Kept for this run only (no settings file)', 'info');
    } catch (e) {
      hiddenSprites = before;
      toast(e instanceof TypeError ? 'Not connected to reforce' : `Not changed: ${e.message}`);
    }
    hiddenChanged();
  }
  const imageSprites = () => libLayer('sprite').configs.filter((c) => !c.movie);
  function updateReviewButton() {
    const b = $('#reviewOpen');
    if (!b) return;
    const n = imageSprites().filter((c) => isHidden(c.id)).length;
    b.title = b.title.replace(/ \(\d+ hidden now\)$/, '') + (n ? ` (${n} hidden now)` : '');
  }

  // The grid, grouped by folder ("Abstract", "External/images"; files at the top level under "Sprites").
  const folderOf = (id) => { const i = id.lastIndexOf('/'); return i > 0 ? id.slice(0, i) : 'Sprites'; };
  let reviewShow = 'all';
  function setupReview() {
    const dlg = $('#review');
    $('#reviewDone').addEventListener('click', () => dlg.close());
    $('#reviewSearch').addEventListener('input', () => renderReview(true));
    for (const b of dlg.querySelectorAll('[data-show]')) {
      b.addEventListener('click', () => {
        reviewShow = b.dataset.show;
        for (const o of dlg.querySelectorAll('[data-show]')) o.setAttribute('aria-pressed', String(o === b));
        renderReview(true);
      });
    }
    const body = $('#reviewBody');
    body.addEventListener('click', (e) => {
      const play = e.target.closest('.rv-play');
      if (play) { post({ layer: 'sprite', action: 'switch', config: play.closest('.rv-tile').dataset.id }); return; }
      const pick = e.target.closest('.rv-pick');
      if (pick) { const id = pick.closest('.rv-tile').dataset.id; setHidden([id], !isHidden(id)); return; }
      const all = e.target.closest('.rv-all');
      if (all) setHidden(JSON.parse(all.dataset.ids), all.dataset.on === '1');
    });
    dlg.addEventListener('keydown', (e) => {
      const pick = e.target.closest && e.target.closest('.rv-pick');
      if (!pick || e.metaKey || e.ctrlKey || e.altKey) return;
      if (e.key === 'p' || e.key === 'P') {
        const tile = pick.closest('.rv-tile');
        if (!isHidden(tile.dataset.id)) post({ layer: 'sprite', action: 'switch', config: tile.dataset.id });
        e.preventDefault();
        return;
      }
      const dirs = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
      const d = dirs[e.key];
      if (!d) return;
      e.preventDefault();
      const picks = [...body.querySelectorAll('.rv-pick')];
      const i = picks.indexOf(pick);
      let next = null;
      if (d[0]) next = picks[i + d[0]];
      else {
        // Up / down: the tile in the next row (across folders) nearest in x.
        const r = pick.getBoundingClientRect();
        const cx = r.left + r.width / 2;
        let best = Infinity, rowTop = null;
        const range = d[1] > 0 ? picks.slice(i + 1) : picks.slice(0, i).reverse();
        for (const p of range) {
          const q = p.getBoundingClientRect();
          if (Math.abs(q.top - r.top) < 2) continue;
          if (rowTop === null) rowTop = q.top;
          if (Math.abs(q.top - rowTop) > 2) break;
          const dx = Math.abs(q.left + q.width / 2 - cx);
          if (dx < best) { best = dx; next = p; }
        }
      }
      if (next) { next.focus(); next.scrollIntoView({ block: 'nearest' }); }
    });
    dlg.addEventListener('close', () => { $('#reviewBody').replaceChildren(); });
  }
  function openReview() {
    const dlg = $('#review');
    $('#reviewSearch').value = '';
    renderReview(true);
    dlg.showModal();
    const first = $('#reviewBody .rv-pick');
    if (first) first.focus();
  }
  // full = rebuild the tiles (filter / search changed); otherwise just restyle them (a hide / show).
  function renderReview(full) {
    const body = $('#reviewBody');
    const all = imageSprites();
    const nHidden = all.filter((c) => isHidden(c.id)).length;
    $('#reviewCount').textContent = `${all.length - nHidden} shown · ${nHidden} hidden`;
    $('#reviewHint').textContent = 'Click an image to hide it. Hidden images never come up, in the slideshow or by hand; ' +
      'click again to bring one back. Keys: arrows move, Space hides / shows, P shows it on screen now.' +
      (hiddenCanSave ? '' : ' (No settings file: changes last for this run.)');
    if (!full) {
      for (const tile of body.querySelectorAll('.rv-tile')) styleTile(tile);
      for (const g of body.querySelectorAll('.rv-group')) styleGroup(g);
      return;
    }
    const focusedId = document.activeElement && document.activeElement.closest('.rv-tile')
      ? document.activeElement.closest('.rv-tile').dataset.id : null;
    const words = $('#reviewSearch').value.toLowerCase().split(/\s+/).filter(Boolean);
    const groups = new Map();
    for (const c of all) {
      if (reviewShow === 'shown' && isHidden(c.id)) continue;
      if (reviewShow === 'hidden' && !isHidden(c.id)) continue;
      if (words.length && !words.every((w) => c.id.toLowerCase().includes(w))) continue;
      const f = folderOf(c.id);
      if (!groups.has(f)) groups.set(f, []);
      groups.get(f).push(c);
    }
    const frag = document.createDocumentFragment();
    for (const [folder, cfgs] of groups) {
      const sec = document.createElement('section');
      sec.className = 'rv-group';
      sec.dataset.ids = JSON.stringify(cfgs.map((c) => c.id));
      const h = document.createElement('h3');
      h.append(document.createTextNode(folder), Object.assign(document.createElement('span'), { className: 'n' }));
      const allBtn = document.createElement('button');
      allBtn.className = 'rv-all';
      allBtn.dataset.ids = sec.dataset.ids;
      h.append(allBtn);
      const grid = document.createElement('div');
      grid.className = 'rv-grid';
      for (const c of cfgs) {
        const tile = document.createElement('div');
        tile.className = 'rv-tile';
        tile.dataset.id = c.id;
        const pick = document.createElement('button');
        pick.className = 'rv-pick';
        const im = document.createElement('span');
        im.className = 'im';
        const img = document.createElement('img');
        img.loading = 'lazy';
        img.decoding = 'async';
        img.alt = '';
        img.src = api(`/api/sprites/image?id=${encodeURIComponent(c.id)}`);
        im.append(img);
        pick.append(im, Object.assign(document.createElement('span'), { className: 'nm', textContent: c.name.trim() }),
          Object.assign(document.createElement('span'), { className: 'badge', textContent: 'Hidden' }));
        const play = document.createElement('button');
        play.className = 'rv-play';
        play.textContent = '▶';
        play.title = 'Show it on screen now';
        play.setAttribute('aria-label', `Show ${c.name.trim()} on screen now`);
        tile.append(pick, play);
        styleTile(tile);
        grid.append(tile);
      }
      sec.append(h, grid);
      styleGroup(sec);
      frag.append(sec);
    }
    if (!groups.size) {
      frag.append(Object.assign(document.createElement('p'), { className: 'rv-empty',
        textContent: !all.length ? 'No image sprites found'
          : reviewShow === 'hidden' && !words.length ? 'Nothing hidden' : 'No match' }));
    }
    body.replaceChildren(frag);
    const back = focusedId && [...body.querySelectorAll('.rv-tile')].find((t) => t.dataset.id === focusedId);
    if (back) $('.rv-pick', back).focus();
  }
  function styleTile(tile) {
    const id = tile.dataset.id;
    const hid = isHidden(id);
    const pick = $('.rv-pick', tile);
    const name = $('.nm', pick).textContent;
    pick.setAttribute('aria-pressed', String(hid));
    pick.title = hid ? `${id}: hidden. Click to bring it back.` : `${id}: click to hide it (never comes up)`;
    pick.setAttribute('aria-label', hid ? `${name}, hidden` : name);
    $('.rv-play', tile).disabled = hid;
  }
  function styleGroup(sec) {
    const ids = JSON.parse(sec.dataset.ids);
    const n = ids.filter(isHidden).length;
    $('.n', sec).textContent = n ? `${ids.length} · ${n} hidden` : String(ids.length);
    const b = $('.rv-all', sec);
    const hideAll = n < ids.length;
    b.textContent = hideAll ? 'Hide all' : 'Show all';
    b.dataset.on = hideAll ? '1' : '0';
    b.title = hideAll ? `Hide every image listed here (${ids.length - n})` : `Bring back every image listed here (${ids.length})`;
  }

  // ------------------------------------------------------------ library list
  function setupLibrary(view, root) {
    const lib = libLayer(layerOf(view.key));
    view.configs = (lib.configs || []).filter((c) =>
      view.key === 'movie' ? !!c.movie : view.key === 'sprite' ? !c.movie : true);
    // Dancers: the generated characters ("Random Disco", ...) first, then the named ones.
    if (view.key === 'dancer') {
      const random = (c) => (/^random /i.test(c.id) ? 0 : 1);
      view.configs = view.configs.map((c, i) => [random(c), i, c]).sort((a, b) => a[0] - b[0] || a[1] - b[1]).map((x) => x[2]);
    }
    view.list = $('.list', root);
    view.search = $('.search', root);
    view.filter = $('.filter', root);
    view.switchBtn = $('.switch', root);
    view.count = $('.count', root);
    view.sel = null;

    const f = view.filter;
    // Objects: browse by group (each one an "Objects: <group>" theme); "All" lists them under group headings.
    view.groups = view.key === 'object' ? (lib.groups || []) : [];
    f.append(new Option(view.groups.length ? 'All groups' : 'All configs', ''));
    for (const g of view.groups) f.append(new Option(g.name, `g:${g.name}`));
    if (view.key === 'movie') f.append(new Option('Beat sync only (♩ BPM)', 'bpm'));
    const saved = store.get(`reforce.filter.${view.key}`);
    if (saved && [...f.options].some((o) => o.value === saved)) f.value = saved;
    f.hidden = f.options.length < 2;  // only "All configs": nothing to pick

    view.search.addEventListener('input', () => renderList(view));
    view.search.addEventListener('keydown', (e) => {
      if (e.key !== 'Enter') return;
      const items = view.list.querySelectorAll('li[data-id]');
      const id = view.sel || (items.length === 1 ? items[0].dataset.id : null);
      if (id) switchTo(view, id);
    });
    f.addEventListener('change', () => {
      store.set(`reforce.filter.${view.key}`, f.value);
      renderList(view);
      updateGroupTheme(view);
    });
    if (view.groups.some((g) => g.theme)) {
      // "Play only <group>": the group's theme (objects from that group only); pressed again: back to Objects.
      const b = document.createElement('button');
      b.className = 'grouptheme';
      b.hidden = true;
      $('.switchrow > span', root).append(b);
      view.groupBtn = b;
      b.addEventListener('click', () => {
        const g = groupOf(view);
        if (!g || !g.theme) return;
        const playing = state && state.theme === g.theme;
        post({ action: 'theme', theme: playing ? 'Objects' : g.theme });
      });
    }
    view.list.addEventListener('click', (e) => {
      const li = e.target.closest('li[data-id]');
      if (!li) return;
      if (e.target.closest('.star')) { toggleFavorite(layerOf(view.key), li.dataset.id); return; }
      select(view, li.dataset.id);
    });
    view.list.addEventListener('dblclick', (e) => {
      if (e.target.closest('.star')) return;
      const li = e.target.closest('li[data-id]');
      if (li) switchTo(view, li.dataset.id);
    });
    view.switchBtn.addEventListener('click', () => { if (view.sel) switchTo(view, view.sel); });
    renderList(view);
    updateGroupTheme(view);
  }

  function groupOf(view) {
    const v = view.filter.value;
    return v.startsWith('g:') ? view.groups.find((g) => `g:${g.name}` === v) : null;
  }

  // The Objects card's group button: shown for a group that has a theme; pressed while that theme plays.
  function updateGroupTheme(view) {
    const b = view.groupBtn;
    if (!b) return;
    const g = groupOf(view);
    b.hidden = !(g && g.theme);
    if (b.hidden) return;
    const playing = !!state && state.theme === g.theme;
    b.setAttribute('aria-pressed', playing ? 'true' : 'false');
    b.classList.toggle('toggle', true);
    b.textContent = playing ? `✓ Playing ${g.name}` : `▶ Play ${g.name}`;
    b.title = playing
      ? `The theme "${g.theme}" is on: the slideshow starts only ${g.name}. Press to go back to the Objects theme (all groups).`
      : `Switch to the theme "${g.theme}": the slideshow starts an object from ${g.name} every 30 s or so, and no others.`;
  }

  function select(view, id) {
    view.sel = id;
    view.list.querySelectorAll('li.sel').forEach((li) => li.classList.remove('sel'));
    const li = [...view.list.children].find((x) => x.dataset.id === id);
    if (li) li.classList.add('sel');
    view.switchBtn.disabled = !id;
  }

  function switchTo(view, id) {
    post({ layer: layerOf(view.key), action: 'switch', config: id });
  }

  // The state a card's list marks as current: the Movie sprites card uses the
  // video state's movie / live sprites (the Sprite layer's own list is images).
  function viewState(view) {
    if (view.key === 'union') {
      // The list marks the Union up next (picked by Prev/Next/the timer), else the last one used.
      const ls = stateLayer('union');
      return ls ? { current: ls.target || ls.current } : ls;
    }
    if (view.key !== 'movie') return stateLayer(view.key);
    const names = (state && state.video && state.video.sprites) || [];
    return { current: names.join(', '), alive: names };
  }

  function matches(cfg, words, filter) {
    if (filter) {
      if (filter === 'bpm') { if (!(cfg.bpm > 0)) return false; }
      else if (filter === 'beat') { if (!cfg.beat) return false; }
      else if (filter === 'nobeat') { if (cfg.beat) return false; }
      else if (filter.startsWith('g:')) { if (!(cfg.categories || []).includes(filter.slice(2))) return false; }
    }
    if (!words.length) return true;
    const hay = cfg.id.toLowerCase();
    return words.every((w) => hay.includes(w));
  }

  function renderList(view) {
    const words = view.search.value.toLowerCase().split(/\s+/).filter(Boolean);
    const filter = view.filter.value;
    const ls = viewState(view);
    const frag = document.createDocumentFragment();
    let n = 0;
    // Objects, all groups: in group order under headings (configs in no group last, under "Other").
    let order = view.configs, heads = null;
    if (!filter && view.groups && view.groups.length) {
      const rank = (c) => {
        const i = view.groups.findIndex((g) => (c.categories || []).includes(g.name));
        return i < 0 ? view.groups.length : i;
      };
      order = view.configs.map((c, i) => [rank(c), i, c]).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
      heads = order.map(([r]) => (r < view.groups.length ? view.groups[r].name : 'Other'));
      order = order.map((x) => x[2]);
    }
    let lastHead = null;
    const hideHidden = layerOf(view.key) === 'sprite';
    let total = 0;
    for (const [idx, cfg] of order.entries()) {
      if (hideHidden && isHidden(cfg.id)) continue;  // hidden sprites: only in Review images
      total++;
      if (!matches(cfg, words, filter)) continue;
      n++;
      if (heads && heads[idx] !== lastHead) {
        lastHead = heads[idx];
        const h = document.createElement('li');
        h.className = 'group';
        h.setAttribute('role', 'presentation');
        h.textContent = lastHead;
        frag.append(h);
      }
      const li = document.createElement('li');
      li.dataset.id = cfg.id;
      li.setAttribute('role', 'option');
      const fav = isFav(layerOf(view.key), cfg.id);
      const star = document.createElement('span');
      star.className = 'star' + (fav ? ' on' : '');
      star.textContent = fav ? '★' : '☆';
      star.title = fav ? 'Remove from favourites' : 'Add to favourites (button at the top)';
      li.append(star);
      // Movies: just the name (their folder is the same for all of them and says nothing).
      const slash = cfg.id.lastIndexOf('/');
      if (slash > 0 && view.key !== 'movie') {
        const d = document.createElement('span');
        d.className = 'dir';
        d.textContent = cfg.id.slice(0, slash + 1);
        li.append(d);
      }
      // The name takes the room the tags (BPM, manual...) don't need.
      li.append(Object.assign(document.createElement('span'), { className: 'nm',
        textContent: view.key === 'movie' ? cfg.name.trim().replace(/_/g, ' ') : cfg.name }));  // (wraps at words)
      if (cfg.parenthesized) {
        li.classList.add('paren');
        const t = document.createElement('span');
        t.className = 'tag';
        t.textContent = 'manual';
        t.title = 'Parenthesized: never picked by the slideshow';
        li.append(t);
      }
      if (cfg.bpm > 0) {
        const t = document.createElement('span');
        t.className = 'tag bpm num';
        t.textContent = String(Math.round(cfg.bpm));  // just the number: the name keeps the width
        t.title = `Beat sync: ${Math.round(cfg.bpm * 10) / 10} BPM (from its .reforce.txt, tools/dance_sync)`;
        li.append(t);
      }
      if (cfg.noColorKey) {
        const t = document.createElement('span');
        t.className = 'tag';
        t.textContent = 'no key';
        t.title = 'ColorKey = 0 in its .reforce.txt: opaque in the Syphon alpha (needs Syphon alpha and Full colour)';
        li.append(t);
      }
      if (cfg.beat) {
        const t = document.createElement('span');
        t.className = 'tag bpm';
        t.textContent = '♩';
        t.title = 'Beat-synced: moves with the beat or tempo (BEAT, TEMPO, LINK_*, SYNC_*)';
        li.append(t);
      }
      if (isCurrent(cfg, ls)) li.classList.add('cur');
      if (cfg.id === view.sel) li.classList.add('sel');
      frag.append(li);
    }
    if (!n) {
      const li = document.createElement('li');
      li.className = 'empty';
      li.textContent = total ? 'No match' : view.configs.length ? 'All hidden (Review images)' : 'No configs found';
      frag.append(li);
    }
    view.list.replaceChildren(frag);
    const nHidden = hideHidden ? view.configs.length - total : 0;
    view.count.textContent = (total ? `${n} / ${total}` : '') + (nHidden ? ` · ${nHidden} hidden` : '');
    view.lastCurrent = ls ? ls.current : '';
  }

  function markCurrent(view, ls) {
    const cur = ls ? ls.current : '';
    if (cur === view.lastCurrent) return;
    view.lastCurrent = cur;
    const byId = new Map(view.configs.map((c) => [c.id, c]));
    for (const li of view.list.children) {
      const cfg = byId.get(li.dataset.id);
      if (cfg) li.classList.toggle('cur', isCurrent(cfg, ls));
    }
  }

  // ------------------------------------------------------------ main layer cards
  function buildLayer(key) {
    const root = $('#layerTpl').content.firstElementChild.cloneNode(true);
    root.dataset.key = key;
    root.id = `panel-${key}`;
    const view = { key, root, editing: {} };
    $('.lname', root).textContent = TITLES[key];
    view.status = $('.status', root);
    view.current = $('.current', root);
    view.target = $('.target', root);
    view.tname = $('.tname', root);
    view.progress = $('.progress', root);
    view.fill = $('.fill', root);
    view.countdown = $('.countdown', root);
    view.blendinfo = $('.blendinfo', root);
    view.hold = $('.hold', root);
    view.fader = $('.fader', root);
    view.xfade = $('.xfade', root);
    view.faderval = $('.faderval', root);
    view.interval = $('.interval', root);
    view.duration = $('.duration', root);
    view.ival = $('.ival', root);
    view.dval = $('.dval', root);
    view.blend = $('.blend', root);

    $('.prev', root).addEventListener('click', () => post({ layer: key, action: 'prev' }));
    $('.next', root).addEventListener('click', () => post({ layer: key, action: 'next' }));
    view.hold.addEventListener('click', () => {
      const ls = stateLayer(key);
      post({ layer: key, action: ls && ls.held ? 'unhold' : 'hold' });
    });

    // Crossfader: dragging takes over the weight; letting go releases it.
    const sendFader = throttle((w) => post({ layer: key, action: 'fader', weight: w }), 60);
    view.xfade.addEventListener('input', () => {
      view.fading = true;
      view.faderval.textContent = Number(view.xfade.value).toFixed(2);
      sendFader(Number(view.xfade.value));
    });
    const release = () => {
      if (!view.fading) return;
      setTimeout(() => {
        post({ layer: key, action: 'release' });
        view.fading = false;
      }, 80);  // after the throttled trailing fader call
    };
    view.xfade.addEventListener('change', release);
    view.xfade.addEventListener('pointerup', release);
    view.xfade.addEventListener('touchend', release);
    view.xfade.addEventListener('blur', release);

    // Interval / transition sliders: label live, send on release.
    const slider = (input, label, action) => {
      const mark = () => { view.editing[action] = Date.now() + 1500; };
      input.addEventListener('pointerdown', mark);
      input.addEventListener('focus', mark);
      input.addEventListener('input', () => { mark(); label.textContent = fmtSec(Number(input.value)); });
      input.addEventListener('change', () => {
        mark();
        post({ layer: key, action, seconds: Number(input.value) });
      });
    };
    slider(view.interval, view.ival, 'interval');
    slider(view.duration, view.dval, 'duration');

    view.blend.addEventListener('change', () => post({ layer: key, action: 'blend', mode: view.blend.value }));

    setupLibrary(view, root);
    views.set(key, view);
    return root;
  }

  function setSlider(view, input, label, action, value) {
    if ((view.editing[action] || 0) > Date.now()) return;
    if (value > Number(input.max)) input.max = String(Math.ceil(value));
    input.value = String(value);
    label.textContent = fmtSec(value);
  }

  function statusText(ls) {
    if (!ls) return ['not active', 'off'];
    if (ls.pending) return ['preparing…', 'morph'];
    if (ls.transitioning) return [`${ls.crossfaderHeld ? 'manual' : 'morph'} ${Math.round((ls.crossfaderHeld ? ls.weight : ls.progress) * 100)}%`, 'morph'];
    if (!ls.slideshowEnabled) return ['slideshow off', 'off'];
    if (ls.held) return ['held', 'held'];
    return ['playing', ''];
  }

  // The parts that move during a transition (status %, progress bar, crossfader): full state and live event.
  function updateProgress(view, ls) {
    const [text, cls] = statusText(ls);
    setText(view.status, text);
    const sc = `chip status ${cls}`;
    if (view.status.className !== sc) view.status.className = sc;
    if (!ls) return;
    view.progress.classList.toggle('pending', !!ls.pending);
    setScale(view.fill, 'X', ls.transitioning ? (ls.crossfaderHeld ? ls.weight : ls.progress) : 0);
    if (!view.fading) {
      const w = ls.transitioning ? ls.weight : 0;
      const wv = String(w);
      if (view.xfade.value !== wv) view.xfade.value = wv;
      setText(view.faderval, ls.transitioning ? w.toFixed(2) : '');
    }
  }

  function updateLayer(view, ls) {
    updateProgress(view, ls);
    if (!ls) {
      setText(view.current, '—');
      return;
    }
    setText(view.current, ls.current || '—');
    view.current.title = ls.current || '';
    const showTarget = (ls.transitioning || ls.pending) && ls.target;
    view.target.classList.toggle('on', !!showTarget);
    setText(view.tname, showTarget ? ls.target : '');

    const extra = [];
    if (ls.morphDepth > 1) extra.push(`depth ${ls.morphDepth}`);
    if (ls.unionName) extra.push(`Union: ${ls.unionName}`);
    else if (ls.unionDue) extra.push('Union next');
    view.blendinfo.textContent = extra.join(' · ');

    view.hold.setAttribute('aria-pressed', ls.held ? 'true' : 'false');
    view.hold.textContent = ls.held ? 'Held' : 'Hold';

    view.fader.classList.toggle('held', !!ls.crossfaderHeld);

    setSlider(view, view.interval, view.ival, 'interval', ls.interval);
    setSlider(view, view.duration, view.dval, 'duration', ls.transitionDuration);

    const modes = ls.blendModes || [];
    const sig = modes.join('|');
    if (view.blendSig !== sig) {
      view.blendSig = sig;
      view.blend.replaceChildren(...modes.map((m) => new Option(m, m)));
      view.blend.closest('.field').hidden = !modes.length;
    }
    if (document.activeElement !== view.blend) view.blend.value = ls.blendMode;

    markCurrent(view, ls);
  }

  function tickCountdowns() {
    if (!state) return;
    const dt = (performance.now() - stateAt) / 1000;
    for (const key of MAIN) {
      const view = views.get(key);
      const ls = stateLayer(key);
      if (!view || !ls) continue;
      let txt = '';
      if (ls.transitioning) txt = `${fmtSec(ls.transitionDuration)} transition`;
      else if (ls.nextSwitchIn >= 0) txt = `next in ${Math.max(0, ls.nextSwitchIn - dt).toFixed(0)} s`;
      else if (ls.held) txt = 'slideshow held';
      setText(view.countdown, txt);
    }
  }

  // ------------------------------------------------------------ extra layers
  function buildExtra(key) {
    const root = $('#extraTpl').content.firstElementChild.cloneNode(true);
    root.dataset.key = key;
    const layer = layerOf(key);
    const view = { key, root, editing: {} };
    $('.lname', root).textContent = key === 'union' ? 'Union' : TITLES[key];
    view.status = $('.status', root);
    view.current = $('.current', root);
    $('.prev', root).addEventListener('click', () => post({ layer, action: 'prev' }));
    $('.next', root).addEventListener('click', () =>
      key === 'movie' ? post({ command: 'nextmovie:sprite' }) : post({ layer, action: 'next' }));
    if (key === 'union') {
      $('.prev', root).title = 'Previous Union for the next FlowField transition';
      $('.next', root).title = 'Next Union for the next FlowField transition';
      view.clear = document.createElement('button');
      view.clear.textContent = 'Clear';
      view.clear.title = 'No Union: cancel a pending one, or morph from a Union blend to the plain FlowField';
      view.clear.addEventListener('click', () => post({ command: 'kill:union' }));
      // Auto: the Union timer (the Union layer's slideshow) on/off.
      view.auto = document.createElement('button');
      view.auto.className = 'toggle';
      view.auto.textContent = 'Auto';
      view.auto.setAttribute('aria-pressed', 'false');
      view.auto.title = 'Union timer on/off: when on, a FlowField transition uses a Union every “Every” seconds';
      view.auto.addEventListener('click', () => {
        const ls = stateLayer('union');
        post({ layer, action: 'slideshow', on: !(ls && ls.slideshowEnabled) });
      });
      $('.row.buttons', root).append(view.clear, view.auto);
      $('summary', root).firstChild.textContent = 'Union library ';
    } else {
      $('.prev', root).hidden = true;  // Particles/Sprites/Objects: "next" starts one more
      $('.next', root).textContent = 'Start next';
      const clear = document.createElement('button');
      clear.textContent = 'Clear';
      const kill = { particle: 'kill:particle', sprite: 'kill:sprite:images', movie: 'kill:sprite:movies',
                     object: 'kill:object', dancer: 'kill:dancer' }[key];
      clear.title = key === 'movie' ? 'Fade out every movie and live video sprite'
        : key === 'sprite' ? 'Fade out every image sprite'
        : key === 'object' ? 'Fade out the objects (trees wither back to the root)'
        : key === 'dancer' ? 'Every dancer waves goodbye and leaves the stage' : 'Fade out every particle';
      if (key === 'object') $('.next', root).title = 'Start the next object (the one showing fades out under it)';
      if (key === 'dancer') $('.next', root).title = 'Bring on the next dancer (with Max dancers on stage, the oldest generated dancer leaves; guests stay)';
      clear.addEventListener('click', () => post({ command: kill }));
      $('.row.buttons', root).append(clear);
      if (key === 'sprite') {
        const review = document.createElement('button');
        review.id = 'reviewOpen';
        review.textContent = 'Review images…';
        review.title = 'Go through every image sprite and hide the ones you don\'t want: hidden images never come up, ' +
          'in the slideshow or by hand. Saved to settings.json (reforce.Sprite.Hidden)';
        review.addEventListener('click', openReview);
        const row = document.createElement('div');
        row.className = 'row buttons';  // its own row: the card is narrow
        row.append(review);
        $('.row.buttons', root).after(row);
      }
      if (key === 'movie') {
        // "Loop until stopped" lives here now (handlers and state stay in the Video code).
        const hold = $('#vHoldSprites');
        if (hold) {
          const row = document.createElement('div');
          row.className = 'row buttons movierow';  // wraps: the checkboxes go under the button in a narrow card
          row.append(hold);
          const beat = $('#vBeatSyncLabel');
          if (beat) row.append(beat);
          const colour = $('#vSpriteColourLabel');
          if (colour) row.append(colour);
          $('.row.buttons', root).after(row);
          // No live (camera / Syphon) sprites here: the camera goes under the visuals as the Camera backdrop
          // (Peter 2026-11-22).
        }
      }
    }
    view.note = document.createElement('div');
    view.note.className = 'note';
    root.insertBefore(view.note, $('.row.buttons', root));
    // Who is alive: below the buttons and collapsed (remembered per browser), so a new one never moves "Start next".
    view.alive = document.createElement('details');
    view.alive.className = 'alive';
    view.alive.hidden = true;
    view.aliveSum = document.createElement('summary');
    view.aliveCount = document.createElement('span');
    view.aliveSum.append(view.aliveCount);
    view.aliveList = document.createElement('div');
    view.aliveList.className = 'alive-list';
    view.alive.append(view.aliveSum, view.aliveList);
    $('.row.buttons', root).after(view.alive);
    rememberOpen(view.alive, `reforce.aliveOpen.${key}`);
    view.countdown = $('.countdown', root);
    const spawn = $('.settings.spawn', root);
    {
      // Spawn settings (Union: its timer): sent (and saved to settings.json) when the slider is released.
      spawn.hidden = false;
      const all = {
        interval: { input: $('.interval', root), label: $('.ival', root), action: 'interval', field: 'interval', fmt: fmtSpan },
        life: { input: $('.life', root), label: $('.lval', root), action: 'life', field: 'spawnDuration', fmt: fmtSec },
        movie: { input: $('.movie-interval', root), label: $('.mval', root), action: 'movieinterval', field: 'movieInterval', fmt: fmtSpan },
      };
      // Sprite Mix (PLAN 5K): one per sprite card; state field spriteMix / movieSpriteMix of the Sprite layer.
      const fmtMix = (x) => x.toFixed(2);
      const mix = { input: $('.mix', root), label: $('.xval', root), action: 'spritemix', fmt: fmtMix,
                    field: key === 'movie' ? 'movieSpriteMix' : 'spriteMix',
                    body: (v) => ({ action: 'spritemix', target: key === 'movie' ? 'movie' : 'sprite', value: v }) };
      if (key === 'union') {
        $('.field.movie', root).remove();
        $('.life', root).closest('.field').remove();
        $('.field.mixfield', root).remove();
        const iv = $('.interval', root);
        iv.min = '10';  // a Union interval of 0 is not "none" (the engine ignores it): Auto turns the timer off
        iv.closest('.field').title = 'How often a FlowField transition uses a Union: seconds from one Union to the next '
          + '(±10%; the Union waits for the next FlowField change). Saved to settings.json '
          + '(FlowField Union.Slideshow.Interval.Duration).';
        $('.ilabel', root).textContent = 'Every';
        view.sliders = [all.interval];
      } else if (key === 'movie') {
        $('.interval', root).closest('.field').remove();
        $('.life', root).closest('.field').remove();
        view.sliders = [all.movie, mix];
        $('.field.movie span', root).firstChild.textContent = 'Interval ';
      } else {
        $('.field.movie', root).remove();
        view.sliders = [all.interval, all.life];
        if (key === 'sprite') view.sliders.push(mix);
        else $('.field.mixfield', root).remove();
        if (key === 'object') {
          $('.interval', root).closest('.field').title = 'Seconds from one slideshow start to the next (±10%); a start '
            + 'waits while an object, organic or solid, or a movie is showing (reforce.Slideshow.OneFeature). '
            + 'At 0 the slideshow starts none. A theme with Object.Slideshow.Interval.Duration (the Objects themes) '
            + 'sets its own; saved to settings.json, this one applies to every other theme.';
          $('.life', root).closest('.field').title = 'Seconds at full strength, between the fade-in and fade-out (±20%). '
            + 'An object\'s own PBar (bars) or PDur wins. Saved to settings.json.';
        }
        if (key === 'dancer') {
          $('.interval', root).closest('.field').title = 'Seconds from one dancer arriving to the next (±10%). '
            + 'At 0 the slideshow brings none. The Dance Party theme sets its own (15 s); saved to settings.json, '
            + 'this one applies to every other theme.';
          $('.life', root).closest('.field').title = 'Seconds each dancer stays on stage (±20%) before waving goodbye. '
            + 'A character\'s own PDur wins. All the way right: ∞, they stay until Clear (or until Max dancers makes '
            + 'way for a new one). Saved to settings.json.';
          // ∞: one step past the slider's top; sent and saved as 0 (Dancer.Duration 0 = forever).
          const life = all.life, top = Number(life.input.max) + 1;
          life.input.max = String(top);
          life.inf = top;
          life.fmt = (x) => (x >= life.inf || x === 0 ? '∞' : fmtSec(x));
          life.body = (x) => ({ layer, action: 'life', seconds: x >= life.inf ? 0 : x });
          setupDancerControls(spawn);
          // No library list: the style buttons make new dancers and Past guests brings back guests.
          $('.library', root).hidden = true;
        }
      }
      for (const sl of view.sliders) {
        const mark = () => { view.editing[sl.action] = Date.now() + 1500; };
        sl.input.addEventListener('pointerdown', mark);
        sl.input.addEventListener('focus', mark);
        sl.input.addEventListener('input', () => { mark(); sl.label.textContent = sl.fmt(Number(sl.input.value)); });
        sl.input.addEventListener('change', () => {
          mark();
          post(sl.body ? sl.body(Number(sl.input.value)) : { layer, action: sl.action, seconds: Number(sl.input.value) });
        });
      }
    }
    setupLibrary(view, root);
    views.set(key, view);
    return root;
  }

  function updateExtra(view, ls) {
    let status = 'not active', cls = 'off', note = '';
    if (view.key === 'union') {
      const ff = stateLayer('flowfield');
      if (ls) {
        status = ls.slideshowEnabled ? (ls.held ? 'held' : 'on timer') : 'slideshow off';
        cls = ls.held ? 'held' : (ls.slideshowEnabled ? '' : 'off');
      }
      const next = ls && ls.target;
      if (ff && ff.unionName) note = ff.transitioning ? `Shaping the current FlowField transition: ${ff.unionName}.`
                                                      : `Holding the blend from ${ff.unionName} until the next FlowField change.`;
      if (next) note = `${note ? `${note} ` : ''}Up next: ${next}.`;
      else if (!note) note = 'Prev/Next or a pick from the list sets the Union the next FlowField transition uses.';
      if (view.clear) view.clear.disabled = !(ff && (ff.unionName || ff.unionDue));
      if (view.auto) view.auto.setAttribute('aria-pressed', ls && ls.slideshowEnabled ? 'true' : 'false');
    } else if (!ls) {
      note = 'Not active in the engine yet; commands are ignored.';
    } else {
      [status, cls] = statusText(ls);
      const alive = viewState(view).alive;
      if (view.alive) {
        view.alive.hidden = !alive;
        if (alive) {
          const label = view.key === 'dancer' ? 'On stage' : 'Alive';
          setText(view.aliveCount, alive.length ? `${label} (${alive.length})` : `${label}: none right now`);
          if (!view.aliveList.hidden) setText(view.aliveList, alive.join(', '));
        }
      }
    }
    const cur = viewState(view);
    view.status.textContent = status;
    view.status.className = `chip status ${cls}`;
    if (view.key === 'union') {
      const ls2 = ls || {};
      const ff = stateLayer('flowfield');
      view.current.textContent = ls2.target ? `Next: ${ls2.target}`
        : ff && ff.unionName ? `Now: ${ff.unionName}`
        : ls2.current ? `Last: ${ls2.current}` : '—';
    } else view.current.textContent = cur && cur.current ? cur.current : '—';
    view.current.title = view.current.textContent;  // one line (ellipsis): the full text on hover
    view.note.textContent = note;
    if (view.sliders && ls) {
      for (const sl of view.sliders) {
        let v = Number(ls[sl.field]);
        if (!Number.isFinite(v) || (view.editing[sl.action] || 0) > Date.now()) continue;
        if (sl.inf && v === 0) v = sl.inf;  // forever: the ∞ step
        else if (sl.inf && v >= sl.inf) {  // a long saved duration: move ∞ up past it
          sl.inf = Math.ceil(v) + 1;
          sl.input.max = String(sl.inf);
        } else if (v > Number(sl.input.max)) sl.input.max = String(Math.ceil(v));
        sl.input.value = String(v);
        sl.label.textContent = sl.fmt(v);
      }
      extraCountdown(view, ls);
    }
    markCurrent(view, cur);
  }

  function extraCountdown(view, ls) {
    if (!view.sliders || !ls || !view.countdown) return;
    {
      let txt = '';
      if (view.key === 'union') {
        const ff = stateLayer('flowfield');
        if (ff && ff.unionDue) txt = 'Union waits for the next FlowField change';
        else if (!ls.playlistSize) txt = 'no Unions in the slideshow';
        else if (!ls.slideshowEnabled) txt = 'timer off';
        else if (ls.held) txt = 'timer held';
        else if (ls.nextSwitchIn >= 0) txt = `next Union in ${Math.round(ls.nextSwitchIn)} s`;
      } else if (view.key === 'movie') {
        if (!ls.moviePlaylistSize) txt = 'no movies in the slideshow';
        else if (ls.movieInterval > 0 && ls.nextMovieIn >= 0) txt = `next movie in ${Math.round(ls.nextMovieIn)} s`;
      } else if (ls.interval > 0 && ls.nextSwitchIn >= 0) {
        const what = { sprite: 'image', object: 'object', dancer: 'dancer' }[view.key] || 'particle';
        txt = `next ${what} in ${Math.round(ls.nextSwitchIn)} s`;
      }
      setText(view.countdown, txt);
    }
  }

  // ------------------------------------------------------------ meters + Link (full state and live event)
  // full: also the tooltips (attribute writes are not free at 20 Hz).
  function updateMeters(au, full) {
    const meters = $('#meters');
    if (!meters) return;
    if (meters.hidden !== !au) meters.hidden = !au;
    if (!au) { setLink(null); return; }
    // BASS / MID / TREBLE are ~0..2 (1 = loud for that band); BEAT is a 0..1 pulse.
    for (const m of meters.querySelectorAll('.meter')) {
      const v = Number(au[m.dataset.k]) || 0;
      setScale(m.firstElementChild, 'Y', v / (m.dataset.k === 'beat' ? 1 : 2));
      if (full) m.title = `${m.dataset.k.toUpperCase()} ${v.toFixed(2)}`;
    }
    setText($('#bpm'), au.tempo > 0 ? `${Number(au.tempo).toFixed(0)} BPM` : '– BPM');
    setLink(au.link, full);
  }

  // Ableton Link (Phase 5H): peers, session tempo and the beat in the bar. The bar
  // position runs on here from the last report and the session tempo (a timer set to
  // the next beat), so the dots step on the beat whatever the push timing.
  let linkRef = null;
  let linkTimer = 0;
  function setLink(lk, full) {
    const lel = $('#link');
    if (lel.hidden !== !lk) lel.hidden = !lk;
    linkRef = lk ? { ...lk, at: performance.now() } : null;
    if (lk) {
      lel.classList.toggle('on', !!lk.on);
      lel.classList.toggle('joining', !lk.on && !!lk.joining);
      setText($('#linkInfo'), lk.on
        ? `${Number(lk.bpm).toFixed(1)} BPM · ${lk.peers} peer${lk.peers === 1 ? '' : 's'}`
        : lk.joining ? 'joining…' : lk.listening ? 'listening' : 'no peers');
      if (full) {
        const n = Math.max(1, Math.round(lk.quantum || 4));
        lel.title = lk.on ? `Ableton Link: linked with ${lk.peers} app${lk.peers === 1 ? '' : 's'}, following the session tempo. Beat ${Number(lk.beat).toFixed(2)}, bar position ${Number(lk.phase).toFixed(2)} of ${n}`
          : lk.joining ? 'Ableton Link: found another Link app, joining its session (reforce takes its tempo)'
          : lk.listening ? 'Ableton Link: listening for rekordbox (Performance mode, LINK on) or another Link app. reforce joins once one runs and follows its tempo'
          : 'Ableton Link: on, no other Link app in the session';
      }
    }
    linkTick();
  }

  function linkTick() {
    clearTimeout(linkTimer);
    const lk = linkRef;
    if (!lk) return;
    const dots = $('#linkBar').children;
    const n = Math.max(1, Math.round(lk.quantum || 4));
    let at = -1;
    if (lk.on && lk.bpm > 0) {
      const msPerBeat = 60000 / lk.bpm;
      const ph = Number(lk.phase) + (performance.now() - lk.at) / msPerBeat;
      at = ((Math.floor(ph) % n) + n) % n;
      // Wake just after the next beat; a newer report (20 Hz) replaces this timer anyway.
      if (!document.hidden) linkTimer = setTimeout(linkTick, (Math.floor(ph) + 1 - ph) * msPerBeat + 4);
    }
    for (let k = 0; k < dots.length; ++k) {
      const hide = k >= n;
      if (dots[k].hidden !== hide) dots[k].hidden = hide;
      dots[k].classList.toggle('lit', k === at);
    }
  }

  // Stats line (fps, sim rate, clock, p99): full state, and fps/clock from the live event.
  function updateStats(s) {
    const parts = [];
    if (s.fps) parts.push(`${s.fps.toFixed(0)} fps`);
    // The sim rate only when it differs from the display rate (keeps the top bar on one row).
    if (!s.fps || Math.abs(Number(s.simRate) - s.fps) >= 1.5) parts.push(`sim ${Number(s.simRate).toFixed(0)} Hz`);
    if (typeof s.simTime === 'number') {
      const t = Math.floor(s.simTime);
      parts.push(`${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`);
    }
    const pf = s.perf;
    if (pf && pf.frames) {
      parts.push(`p99 ${Number(pf.p99).toFixed(1)} ms`);
      parts.push(`${pf.hitches} hitch${pf.hitches === 1 ? '' : 'es'}`);
    }
    const st = $('#stats');
    setText(st, parts.join(' · '));
    if (st.title !== st.textContent) st.title = st.textContent;
  }

  // ------------------------------------------------------------ global bar
  function setupGlobal() {
    const theme = $('#theme');
    theme.addEventListener('change', () => post({ action: 'theme', theme: theme.value }));
    $('#slideshowAll').addEventListener('click', () => {
      const on = !(state && state.slideshow && state.slideshow.allOn);
      post({ layer: 'all', action: 'slideshow', on });
    });
    $('#holdAll').addEventListener('click', () => {
      const held = state && state.slideshow && state.slideshow.allHeld;
      post({ layer: 'all', action: held ? 'unhold' : 'hold' });
    });
    $('#lights').addEventListener('click', () => {
      const on = !(state && state.video && state.video.lights && state.video.lights.on);
      post({ action: 'videoset', key: 'lights', value: on });
    });
    $('#windowVisuals').addEventListener('click', () => {
      const on = !(state && state.video && state.video.windowVisuals);
      post({ action: 'videoset', key: 'windowvisuals', value: on });
    });
  }

  function updateGlobal(s) {
    const theme = $('#theme');
    const names = ['(Standard)', ...(s.themes && s.themes.length ? s.themes : library.themes)];
    const sig = names.join('|');
    if (theme.dataset.sig !== sig) {
      theme.dataset.sig = sig;
      theme.replaceChildren(...names.map((n) => new Option(n, n)));
    }
    if (document.activeElement !== theme) theme.value = s.theme;
    const ov = views.get('object');
    if (ov) updateGroupTheme(ov);

    const ss = s.slideshow || {};
    const sa = $('#slideshowAll');
    sa.setAttribute('aria-pressed', ss.allOn ? 'true' : 'false');
    sa.textContent = ss.allOn ? 'Slideshow on' : (ss.anyOn ? 'Slideshow partly on' : 'Slideshow off');
    const ha = $('#holdAll');
    ha.setAttribute('aria-pressed', ss.allHeld ? 'true' : 'false');
    ha.textContent = ss.allHeld ? 'All held' : 'Hold all';
    const wv = $('#windowVisuals');
    const visualsOnly = !!(s.video && s.video.windowVisuals);
    wv.setAttribute('aria-pressed', visualsOnly ? 'true' : 'false');
    // Room lights: lit when on; a mark when on but not streaming (connecting, not paired, lost), the reason on hover.
    const lt = $('#lights');
    const L = (s.video && s.video.lights) || {};
    const trouble = L.on && L.state && L.state !== 'streaming';
    lt.disabled = !L.available;
    lt.setAttribute('aria-pressed', L.on ? 'true' : 'false');
    lt.textContent = trouble ? (L.state === 'connecting' ? 'Lights …' : 'Lights ⚠') : 'Lights';
    lt.title = !L.available ? 'Room lights: not in this instance (a test instance needs --lights)'
      : 'Room lights (Nanoleaf): ' + (L.on ? (L.state === 'streaming'
        ? `streaming to ${L.device} (${L.panels} panels, ${Math.round(L.hz || 0)}/s)` : `${L.state}${L.message ? ': ' + L.message : ''}`)
        : 'off') + '. The panels follow the visuals (not the camera, dancers or text). Also in the View menu. Saved to settings.json (reforce.Lights.Enabled)';

    updateStats(s);
    const pf = s.perf;
    const hist = $('#perfHist');
    if (hist) {
      if (pf && pf.hist && pf.hist.length) {
        // Compact frame-time histogram (max(cpu, gpu) per frame): one bar per bin, red past 1.5x budget.
        const peak = Math.max(1, ...pf.hist);
        const bars = '▁▂▃▄▅▆▇█';
        hist.replaceChildren(...pf.hist.map((c, i) => {
          const span = document.createElement('span');
          span.textContent = c ? bars[Math.min(7, Math.floor((7 * Math.sqrt(c)) / Math.sqrt(peak)))] : ' ';
          const lo = i ? pf.histEdges[i - 1] : 0;
          const hi = i < pf.histEdges.length ? pf.histEdges[i] : Infinity;
          span.title = `${lo.toFixed(1)}–${hi === Infinity ? '∞' : hi.toFixed(1)} ms: ${c} frames`;
          if (lo >= 1.5 * pf.budget - 0.01) span.className = 'over';
          return span;
        }));
        hist.title = `frame work time (max of CPU and GPU), budget ${Number(pf.budget).toFixed(1)} ms` +
          (pf.lastHitch ? `\nlast hitch: ${pf.lastHitch}` : '');
        hist.hidden = false;
      } else {
        hist.hidden = true;
      }
    }

    updateMeters(s.audio, true);

    const log = s.log || [];
    const lsig = log.length ? log[log.length - 1] + log.length : '';
    const ol = $('#log');
    if (ol.dataset.sig !== lsig) {
      ol.dataset.sig = lsig;
      ol.replaceChildren(...log.slice().reverse().map((line) => {
        const li = document.createElement('li');
        li.textContent = line;
        return li;
      }));
    }
  }

  // ------------------------------------------------------------ tabs (phone)
  function setupTabs() {
    const tabs = $('#tabs');
    let active = store.get('reforce.tab');
    if (!MAIN.includes(active)) active = MAIN[0];
    const activate = (key) => {
      store.set('reforce.tab', key);
      for (const b of tabs.children) b.setAttribute('aria-selected', b.dataset.key === key ? 'true' : 'false');
      for (const k of MAIN) views.get(k).root.classList.toggle('active', k === key);
    };
    for (const key of MAIN) {
      const b = document.createElement('button');
      b.dataset.key = key;
      b.setAttribute('role', 'tab');
      b.setAttribute('aria-controls', `panel-${key}`);
      b.textContent = TITLES[key];
      b.addEventListener('click', () => activate(key));
      tabs.append(b);
    }
    activate(active);
    const bar = $('.bar');
    const setBarH = () => document.documentElement.style.setProperty('--barh', `${bar.offsetHeight}px`);
    setBarH();
    window.addEventListener('resize', setBarH);
  }

  // ------------------------------------------------------------ video (Phase 5F)
  const vset = (key, value) => post({ action: 'videoset', key, value });
  // Beat offset nudge (top bar): local value while clicking, then the engine's.
  let boValue = 0, boEditUntil = 0;
  const boShow = () => { $('#boVal').textContent = `${boValue >= 0 ? '+' : ''}${boValue} ms`; };
  const boSend = throttle(() => vset('beatoffset', String(boValue)), 150);
  const boNudge = (d) => {
    boValue = Math.max(-200, Math.min(500, boValue + d));
    boEditUntil = Date.now() + 1500;
    boShow();
    boSend();
  };
  $('#boMinus').addEventListener('click', (e) => boNudge(e.shiftKey ? -1 : -5));
  $('#boPlus').addEventListener('click', (e) => boNudge(e.shiftKey ? 1 : 5));
  $('#boVal').addEventListener('dblclick', () => boNudge(-boValue));
  function updateBeatOffset(v) {
    if (!v || !Number.isFinite(Number(v.beatOffsetMs)) || Date.now() < boEditUntil) return;
    boValue = Math.round(Number(v.beatOffsetMs));
    boShow();
  }
  const vEditing = new Set();  // inputs the user is dragging: state updates leave them alone
  let cameraPerm = 'ask';      // from /api/video: ask | waiting | granted | denied | unavailable
  let vSourcesAt = 0;          // last /api/video fetch (refreshed while a camera waits for permission)

  // A video-card control from the state, unless the user is on it right now; its "<id>Val" label too.
  function setVal(id, value, fmt) {
    const input = $('#' + id);
    if (vEditing.has(input) || document.activeElement === input) return;
    input.value = value;
    const label = $('#' + id + 'Val');
    if (label && fmt) label.textContent = fmt(value);
  }

  // A source picker: option groups [label, items, text(item)] (items with .source; our own Syphon output disabled),
  // the previous pick kept when it is still there. Returns false when there is nothing to pick.
  function fillSources(sel, groups, emptyText, keep) {
    sel.textContent = '';
    for (const [label, items, text] of groups) {
      if (!items || !items.length) continue;
      const g = document.createElement('optgroup');
      g.label = label;
      for (const it of items) {
        const o = new Option(text(it), it.source);
        if (it.self) { o.disabled = true; o.textContent += ' (our own output)'; }
        g.append(o);
      }
      sel.append(g);
    }
    const any = sel.options.length > 0;
    if (!any) sel.append(Object.assign(new Option(emptyText, ''), { disabled: true }));
    if ([...sel.options].some((o) => o.value === keep && !o.disabled)) sel.value = keep;
    return any;
  }

  function updateCameraHint(error) {
    const denied = cameraPerm === 'denied' || /camera access denied/.test(error || '');
    const hint = $('#vCamera');
    hint.hidden = !(denied || cameraPerm === 'unavailable');
    $('#vCameraLink').hidden = !denied;
    $('#vCameraText').textContent = cameraPerm === 'unavailable'
      ? 'This reforce.app was built without a camera usage description: reinstall it (scripts/install-app.sh). '
      : 'Camera access is off for reforce. Turn it on, then pick the camera again. ';
  }

  // Camera backdrop (docs/backdrop.md): cameras first, then Syphon servers and movie files.
  function fillBackdropSources(v, camNote) {
    const sel = $('#bdSource');
    if (!sel) return;
    const cur = state && state.video && state.video.backdrop ? state.video.backdrop.spec : '';
    // Cameras by name, not unique id: a USB camera's id changes with the port it is plugged into.
    const cams = (v.cameras || []).map((c) => ({ ...c, source: 'camera:' + c.name }));
    const any = fillSources(sel, [['Cameras' + camNote, cams, (c) => c.name], ['Syphon', v.syphon, (x) => x.display],
                                  ['Files', v.files, (f) => f.id]],
                            'No cameras, Syphon servers or movies', sel.value || cur || store.get('reforce.backdropSource') || '');
    $('#bdStart').disabled = !any;
  }

  async function loadVideoSources() {
    const sel = $('#vSource');
    try {
      const r = await fetch(api('/api/video'));
      if (!r.ok) return;
      const v = await r.json();
      vSourcesAt = performance.now();
      cameraPerm = v.cameraPermission || 'ask';
      updateCameraHint(state && state.video ? state.video.error : '');
      const camNote = { denied: ' (access denied: System Settings › Privacy & Security › Camera)',
        waiting: ' (waiting for permission…)', unavailable: ' (unavailable: reinstall reforce.app)' }[cameraPerm] || '';
      fillSources(sel, [['Files', v.files, (f) => f.id], ['Cameras' + camNote, v.cameras, (c) => c.name],
                        ['Syphon', v.syphon, (x) => x.display]],
                  'No sources (Sprite package movies, reforce.VideoDir, cameras, Syphon)',
                  sel.value || store.get('reforce.videoSource') || '');
      fillBackdropSources(v, camNote);
    } catch { /* not connected */ }
  }

  // Page scrolling does not get caught on the scene lists (Peter, 2026-11-12):
  // while you are scrolling the page, a list that slides under the pointer does
  // not take the wheel. A list scrolls once you pause about half a second with
  // the pointer on it, or move the pointer onto it on purpose.
  const SCROLL_GRACE_MS = 500;
  function setupScrollIntent() {
    let lastPageWheel = -1e9;
    let intent = null;  // the list the pointer was really moved onto
    document.addEventListener('mousemove', (e) => {
      if (!e.movementX && !e.movementY) return;  // synthetic moves while content scrolls under the pointer
      intent = e.target.closest ? e.target.closest('.list') : null;
    }, { passive: true });
    document.addEventListener('wheel', (e) => {
      if (e.ctrlKey) return;  // pinch zoom
      const now = performance.now();
      const list = e.target.closest ? e.target.closest('.list') : null;
      const scrollable = list && list.scrollHeight > list.clientHeight + 1;
      if (!scrollable) {
        lastPageWheel = now;
        intent = null;  // scrolling the page from elsewhere: a list must be pointed at again
        return;
      }
      if (list === intent || now - lastPageWheel > SCROLL_GRACE_MS) return;  // the list scrolls
      e.preventDefault();
      const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? window.innerHeight : 1;
      window.scrollBy(0, e.deltaY * unit);
      lastPageWheel = now;
    }, { passive: false, capture: true });
  }

  function setupVideo() {
    const sel = $('#vSource');
    sel.addEventListener('change', () => store.set('reforce.videoSource', sel.value));
    $('#vRefresh').addEventListener('click', loadVideoSources);
    $('#bdRefresh').addEventListener('click', loadVideoSources);
    const play = (target) => () => {
      if (!sel.value) { toast('Pick a video source first'); return; }
      post({ action: 'video', target, source: sel.value });
    };
    $('#vPlayLayer').addEventListener('click', play('layer'));
    // The Video layer section opens on demand (most sets never use it); remembered per browser.
    rememberOpen($('#videoWrap'), 'reforce.videoOpen');
    $('#vStopLayer').addEventListener('click', () => post({ action: 'videostop', target: 'layer' }));
    $('#vHoldSprites').addEventListener('click', () => post({ action: 'videohold' }));
    $('#vBeatSync').addEventListener('change', (e) => vset('beatsync', e.target.checked));
    $('#vSpriteColour').addEventListener('change', (e) => vset('spritecolour', e.target.checked ? 'over' : 'off'));
    $('#vSyphonAlpha').addEventListener('change', (e) => vset('syphonalpha', e.target.checked));
    const slider = (id, key, fmt) => {
      const input = $('#' + id);
      const label = $('#' + id + 'Val');
      const send = throttle(() => vset(key, Number(input.value)), 80);
      input.addEventListener('input', () => { vEditing.add(input); if (label) label.textContent = fmt(Number(input.value)); send(); });
      input.addEventListener('change', () => { vEditing.delete(input); vset(key, Number(input.value)); });
    };
    const num = (x) => x.toFixed(2);
    slider('vOpacity', 'opacity', pct);
    slider('vXfade', 'crossfade', fmtSec);
    slider('vThreshold', 'threshold', num);
    slider('vTolerance', 'tolerance', num);
    slider('vSoftness', 'softness', num);
    slider('vRate', 'rate', (x) => x.toFixed(2) + '×');
    slider('vKeySim', 'keysimilarity', fmtInt);
    slider('vKeySmooth', 'keysmoothness', fmtInt);
    // Room lights (docs/lights.md)
    rememberOpen($('#ltWrap'), 'reforce.lightsOpen');
    slider('ltBrightness', 'lightsbrightness', pct);
    slider('ltSaturation', 'lightssaturation', (x) => x.toFixed(2) + '×');
    slider('ltFloor', 'lightsfloor', pct);
    slider('ltSmoothing', 'lightssmoothing', fmtSec);
    $('#ltOn').addEventListener('click', () => {
      const L = (state && state.video && state.video.lights) || {};
      vset('lights', !L.on);
    });
    $('#ltPair').addEventListener('click', () => {
      const L = (state && state.video && state.video.lights) || {};
      vset('lightspair', L.state === 'pairing' ? 'cancel' : 'start');
    });
    for (const [id, key] of [['vBlend', 'blend'], ['vOrder', 'order'], ['vFit', 'fit'], ['vKey', 'key']])
      $('#' + id).addEventListener('change', (e) => vset(key, e.target.value));
    $('#vKeyColor').addEventListener('change', (e) => vset('keycolor', e.target.value));
    $('#vLoop').addEventListener('change', (e) => vset('loop', e.target.checked));
    $('#vSyphon').addEventListener('change', (e) => vset('syphon', e.target.checked));
    // Camera backdrop
    const bdSel = $('#bdSource');
    bdSel.addEventListener('change', () => store.set('reforce.backdropSource', bdSel.value));
    $('#bdStart').addEventListener('click', () => {
      if (!bdSel.value) { toast('Pick a camera first'); return; }
      vset('backdrop', bdSel.value);
    });
    $('#bdStop').addEventListener('click', () => vset('backdrop', 'off'));
    $('#bdMirror').addEventListener('change', (e) => vset('backdropmirror', e.target.checked));
    $('#bdFit').addEventListener('change', (e) => vset('backdropfit', e.target.value));
    slider('bdOpacity', 'backdropopacity', pct);
    slider('bdKeySim', 'keysimilarity', fmtInt);
    slider('bdKeySmooth', 'keysmoothness', fmtInt);
    // Body tracking (docs/body.md)
    const bset = (key, value) => vset('body' + key, value);
    $('#bodyTrack').addEventListener('change', (e) => bset('track', e.target.checked));
    $('#bodyShow').addEventListener('change', (e) => bset('show', e.target.checked));
    $('#bodySync').addEventListener('change', (e) => bset('sync', e.target.checked));
    $('#bodyQuality').addEventListener('change', (e) => bset('quality', e.target.value));
    $('#bodyBurn').addEventListener('change', (e) => bset('burn', e.target.value));
    slider('bodyFront', 'bodyfront', pct);
    slider('bodyTrails', 'bodytrails', pct);
    slider('bodyTrailWidth', 'bodytrailwidth', (x) => Math.round(x) + ' px');
    slider('bodyBurnAmount', 'bodyburnamount', pct);
    rememberOpen($('#bodyWrap'), 'reforce.bodyOpen');
    rememberOpen($('#bdWrap'), 'reforce.backdropOpen');
    loadVideoSources();
  }

  function updateBody(v) {
    const b = v.body;
    if (!b) return;
    const backdrop = v.backdrop && v.backdrop.on;
    const status = $('#bodyStatus');
    const seen = b.running && b.presence > 0.5;
    status.textContent = !b.track ? 'off' : !backdrop ? 'no backdrop' : !b.running ? 'starting' : seen ? 'person' : 'nobody';
    status.className = 'chip status' + (seen ? ' on' : '');
    setText($('#bodySummary'), !b.track ? 'off' : !backdrop ? 'on: needs the camera backdrop' : seen ? 'on: tracking' : 'on');
    // Green while on and working (tracking the backdrop), like the camera backdrop and room lights; not only with a person seen.
    $('#bodySummary').classList.toggle('on', !!(b.track && backdrop));
    let meta;
    if (!backdrop) meta = 'Tracks the camera backdrop: turn that on first.';
    else if (!b.track) meta = 'Tick Track to follow the person on the backdrop.';
    else if (!b.running || (!b.fps && !b.mask)) meta = 'Starting (Vision loads its models, about a second)…';
    else {
      const who = !seen ? 'nobody in view' : b.poseMs > 0 ? `person · ${b.joints} of 19 joints` : 'person';
      const vision = `silhouette ${(b.maskMs || 0).toFixed(1)} ms` + (b.poseMs > 0 ? ` · pose ${b.poseMs.toFixed(1)} ms` : '');
      meta = `${who} · tracking ${Math.round(b.fps)} of ${Math.round(b.camFps || 0)} camera fps · ${vision}`;
    }
    setText($('#bodyMeta'), meta);
    $('#bodyTrack').checked = !!b.track;
    $('#bodyShow').checked = !!b.show;
    $('#bodySync').checked = b.sync !== false;
    setVal('bodyQuality', b.quality);
    setVal('bodyBurn', b.burn);
    setVal('bodyFront', b.front, pct);
    setVal('bodyTrails', b.trails, pct);
    setVal('bodyTrailWidth', b.trailWidth, (x) => Math.round(x) + ' px');
    setVal('bodyBurnAmount', b.burnAmount, pct);
    const err = $('#bodyError');
    err.hidden = !b.error;
    err.textContent = b.error || '';
  }

  function updateBackdrop(v) {
    const b = v.backdrop;
    if (!b) return;
    const showing = b.on && !!b.source && (b.level === undefined || b.level > 0);
    const label = b.pending ? `opening ${b.pending}…` : b.source || (b.on ? b.spec : '');
    const status = $('#bdStatus');
    status.textContent = b.waiting ? 'waiting…' : b.pending ? 'opening…' : showing ? 'on' : b.on ? 'starting' : b.source ? 'fading out' : 'off';
    status.className = 'chip status' + (showing ? ' on' : '');
    setText($('#bdSummary'), b.on ? `on: ${b.source || b.pending || b.spec}` : 'off');
    $('#bdSummary').classList.toggle('on', showing);
    setText($('#bdNow'), label || '–');
    const meta = [];
    if (b.width) meta.push(`${b.width}×${b.height}`);
    if (b.waiting) meta.push(`⏳ ${b.waiting}`);
    setText($('#bdMeta'), meta.join(' · '));
    setVal('bdOpacity', b.opacity, pct);
    setVal('bdFit', b.fit);
    if (v.keySimilarity !== undefined) setVal('bdKeySim', v.keySimilarity, fmtInt);
    if (v.keySmoothness !== undefined) setVal('bdKeySmooth', v.keySmoothness, fmtInt);
    $('#bdMirror').checked = !!b.mirror;
    const err = $('#bdError');
    const e = /^backdrop:/.test(v.error || '') ? v.error : '';
    err.hidden = !e;
    err.textContent = e;
  }

  // ------------------------------------------------------------ room lights (docs/lights.md)
  let ltSig = '';
  // The panels as they hang (Nanoleaf layout: y up; a triangle points up at 0°, down at 60°), filled with the colours
  // last sent. Built once per layout, recoloured from the live events.
  function drawLightsPreview(L) {
    const svg = $('#ltPreview');
    const lay = L.layout || [];
    const sig = `${L.side}|${lay.join(',')}`;
    if (sig !== ltSig) {
      ltSig = sig;
      const ns = 'http://www.w3.org/2000/svg';
      const R = (L.side || 150) / Math.sqrt(3);
      const polys = [];
      let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
      for (let i = 0; i + 2 < lay.length; i += 3) {
        const [x, y, o] = [lay[i], lay[i + 1], lay[i + 2]];
        const pts = [0, 1, 2].map((k) => {
          const a = ((90 + o + k * 120) * Math.PI) / 180;
          const px = x + R * 0.93 * Math.cos(a), py = y + R * 0.93 * Math.sin(a);
          x0 = Math.min(x0, px); x1 = Math.max(x1, px); y0 = Math.min(y0, py); y1 = Math.max(y1, py);
          return [px, py];
        });
        polys.push(pts);
      }
      svg.replaceChildren();
      if (!polys.length) { svg.hidden = true; return; }
      svg.hidden = false;
      const pad = 8;
      svg.setAttribute('viewBox', `${x0 - pad} ${-y1 - pad} ${x1 - x0 + 2 * pad} ${y1 - y0 + 2 * pad}`);
      for (const pts of polys) {
        const p = document.createElementNS(ns, 'polygon');
        p.setAttribute('points', pts.map(([px, py]) => `${px.toFixed(1)},${(-py).toFixed(1)}`).join(' '));
        p.setAttribute('fill', '#000');
        svg.appendChild(p);
      }
    }
    const c = L.on && L.state === 'streaming' ? (L.colours || '') : '';
    const polys = svg.children;
    for (let i = 0; i < polys.length; i++) {
      const hex = c.length >= (i + 1) * 6 ? '#' + c.slice(i * 6, i * 6 + 6) : '#000000';
      if (polys[i].getAttribute('fill') !== hex) polys[i].setAttribute('fill', hex);
    }
  }

  function updateLights(v) {
    const L = v.lights;
    if (!L) return;
    $('#lightsCard').hidden = !L.available && !L.on;
    const streaming = L.on && L.state === 'streaming';
    const status = $('#ltStatus');
    status.textContent = !L.available ? 'not here' : L.state === 'pairing' ? 'pairing…' : streaming ? (L.pulsing ? 'on · pulse' : 'on')
      : L.on ? (L.state === 'connecting' ? 'connecting…' : L.state) : 'off';
    status.className = 'chip status' + (streaming ? ' on' : '');
    setText($('#ltSummary'), streaming ? `on: ${L.device}` : L.on ? L.state : 'off');
    $('#ltSummary').classList.toggle('on', streaming);
    setText($('#ltNow'), L.device ? `${L.device}` : L.available ? 'Nanoleaf panels' : 'Not in this instance (a test instance needs --lights)');
    const meta = [];
    if (L.panels) meta.push(`${L.panels} panels`);
    if (streaming && L.hz) meta.push(`${Math.round(L.hz)} updates/s`);
    if (streaming && L.pulse > 0) meta.push(L.pulsing ? 'pulsing on the beat' : 'pulse waits for a confident beat');
    setText($('#ltMeta'), meta.join(' · '));
    const on = $('#ltOn');
    on.disabled = !L.available;
    on.textContent = L.on ? 'Turn off' : 'Turn on';
    on.classList.toggle('primary', !L.on);
    const pair = $('#ltPair');
    pair.disabled = !L.available;
    pair.textContent = L.state === 'pairing' ? 'Cancel pairing' : 'Pair…';
    const hint = $('#ltHint');
    const msg = L.message || '';
    hint.hidden = !msg || streaming;
    hint.textContent = msg;
    setVal('ltBrightness', L.brightness, pct);
    setVal('ltSaturation', L.saturation, (x) => x.toFixed(2) + '×');
    setVal('ltFloor', L.floor, pct);
    setVal('ltSmoothing', L.smoothing, fmtSec);
    drawLightsPreview(L);
  }

  function updateVideo(v) {
    if (!v) return;
    updateLights(v);
    updateBackdrop(v);
    updateBody(v);
    updateBeatOffset(v);
    const status = $('#vStatus');
    status.textContent = v.waiting && !v.active ? 'waiting…' : v.pending ? 'opening…' : v.crossfade >= 0 ? 'crossfade' : v.active ? 'on' : 'off';
    const waiting = $('#vWaiting');
    waiting.hidden = !v.waiting;
    waiting.textContent = v.waiting ? `⏳ ${v.waiting}` : '';
    // A camera waiting for (or just refused) permission: refresh the source
    // list now and then so the permission state shown stays current.
    const camState = /camera permission|camera access/.test((v.waiting || '') + (v.error || ''));
    if (camState && performance.now() - vSourcesAt > 3000) loadVideoSources();
    status.className = 'chip status' + (v.active ? ' on' : '');
    // Summary line of the closed Video layer section: whether something plays on the layer.
    setText($('#vSummary'), v.active || v.pending || v.incoming
      ? `on: ${v.incoming || v.source || v.pending}` : v.waiting ? 'waiting…' : 'off');
    $('#vSummary').classList.toggle('on', !!v.active);
    $('#vNow').textContent = v.incoming ? `${v.source || '–'} → ${v.incoming}` : (v.source || (v.pending ? `opening ${v.pending}…` : '–'));
    const meta = [];
    if (v.width) meta.push(`${v.width}×${v.height}`);
    if (v.fps) meta.push(`${v.fps.toFixed(2)} fps`);
    if (v.decodeMsAvg) meta.push(`decode ${v.decodeMsAvg.toFixed(2)} ms (max ${v.decodeMsMax.toFixed(1)})`);
    if (v.dropped) meta.push(`${v.dropped} dropped`);
    if (v.sprites && v.sprites.length) meta.push(`sprites: ${v.sprites.join(', ')}`);
    const hold = $('#vHoldSprites');
    hold.disabled = !(v.sprites && v.sprites.length) || !!v.spritesHeld;
    hold.textContent = v.spritesHeld ? '⟳ Looping until stopped' : '⟳ Loop until stopped';
    $('#vMeta').textContent = meta.join(' · ');
    setScale($('#vFade'), 'X', v.crossfade >= 0 ? v.crossfade : 0);
    setVal('vOpacity', v.opacity, pct);
    if (v.keySimilarity !== undefined) setVal('vKeySim', v.keySimilarity, fmtInt);
    if (v.keySmoothness !== undefined) setVal('vKeySmooth', v.keySmoothness, fmtInt);
    $('#vSyphonAlpha').checked = !!v.syphonAlpha;
    $('#vSyphonKey').hidden = !v.syphonAlpha;
    setVal('vXfade', v.crossfadeSeconds, fmtSec);
    setVal('vThreshold', v.threshold, (x) => x.toFixed(2));
    setVal('vTolerance', v.tolerance, (x) => x.toFixed(2));
    setVal('vSoftness', v.softness, (x) => x.toFixed(2));
    setVal('vRate', v.rate, (x) => x.toFixed(2) + '×');
    setVal('vBlend', v.blend);
    setVal('vOrder', v.order);
    setVal('vFit', v.fit);
    setVal('vKey', v.key);
    setVal('vKeyColor', v.keyColor);
    $('#vLoop').checked = v.loop;
    $('#vBeatSync').checked = !!v.beatSync;
    $('#vSpriteColour').checked = v.spriteColour === 'over';
    const clock = v.beatSource === 'none' ? 'no beat clock'
      : `${v.beatSource === 'link' ? 'Link' : 'audio'}${v.beatBpm ? ' ' + v.beatBpm.toFixed(1) : ''}${v.beatConfident ? '' : ' (not locked)'}`;
    $('#vBeatClock').textContent = clock;
    $('#vSyphon').checked = v.syphon;
    for (const el of document.querySelectorAll('.vkey-luma')) el.hidden = v.key !== 'luma';
    for (const el of document.querySelectorAll('.vkey-chroma')) el.hidden = v.key !== 'chroma';
    for (const el of document.querySelectorAll('.vkey-any')) el.hidden = v.key === 'none';
    const err = $('#vError');
    err.hidden = !v.error;
    err.textContent = v.error || '';
    updateCameraHint(v.error);
  }


  // ------------------------------------------------------------ look
  // Rendering-quality choices (engine/Look.hpp; PLAN "Fidelity choices").
  // tag: 'gf' = matches G-Force, 'ok' = reforce / precise.
  const LOOK = [
    { id: 'thinlines', title: 'Thin lines & dots', setting: 'reforce.ThinLines', values: [
      { v: 'gforce', label: 'G-Force', tag: 'gf', tagText: 'G-Force',
        note: 'Snapped to pixels like G-Force: a thin line is one hard pixel row, small dots are 1-px boxes; crisp, but they step and shimmer as they move.' },
      { v: 'smooth', label: 'Smooth', tag: 'ok', tagText: 'reforce',
        note: 'Antialiased round dots and sub-pixel lines with G-Force’s brightness: smoother motion, softer edges.' },
    ] },
    { id: 'intensity', title: 'Trail buffer', setting: 'reforce.Intensity', values: [
      { v: 'gforce', label: '8-bit', tag: 'gf', tagText: 'G-Force',
        note: '8-bit like G-Force: slow zooms “stick”, gradients band and trails fade out in visible steps.' },
      { v: 'hq', label: '16-bit float', tag: 'ok', tagText: 'precise',
        note: 'No banding or sticking: slow zooms keep moving, glows stay smooth; trail length can differ slightly.' },
    ] },
    { id: 'flowrounding', title: 'Flow resampling', setting: 'reforce.FlowRounding', values: [
      { v: 'fixed16', label: '1/16 px, truncate', tag: 'gf', tagText: 'G-Force',
        note: 'G-Force’s integer bilinear (1/16-px weights, truncated): trails a little darker and shorter.' },
      { v: 'nearest', label: 'Float, nearest', tag: 'ok', tagText: 'precise',
        note: 'Float bilinear, rounded to nearest: trails a little brighter and longer, no downward drift.' },
    ], advanced: [
      { v: 'floor', label: 'float + truncate', note: 'Float bilinear, truncated to the 8-bit level (experiment).' },
      { v: 'point', label: 'point + truncate', note: 'Nearest-pixel sampling, truncated: blocky, jittery flow (experiment).' },
      { v: 'fixed256', label: '1/256 px, truncate', note: 'Like G-Force but with finer 1/256-px weights (experiment).' },
    ] },
    { id: 'flowprecision', title: 'FlowField math', setting: 'reforce.FlowPrecision', rebuild: true, values: [
      { v: 'double', label: 'Double', tag: 'ok', tagText: 'reforce',
        note: 'Double precision. Over all 352 fields float32 measured as a wash, so expect no visible change.' },
      { v: 'float32', label: 'Float32', tag: '', tagText: 'G-Force?',
        note: 'Single precision, a guess at G-Force’s math; differences are tiny (Nebula 0.58 → 0.55).' },
    ] },
    { id: 'aa', title: 'Line antialiasing', setting: 'WaveShape.AntiAlias', values: [
      { v: 'on', label: 'On', tag: 'gf', tagText: 'G-Force default',
        note: 'Antialiased line edges (G-Force’s default, WaveShape.AntiAlias = 1).' },
      { v: 'off', label: 'Off', tag: '', tagText: '',
        note: 'Hard, aliased line edges (WaveShape.AntiAlias = 0).' },
    ] },
    { id: 'linescale', title: 'Line width scaling', setting: 'reforce.LineScale', values: [
      { v: 'gforce', label: 'G-Force', tag: 'gf', tagText: 'G-Force',
        note: 'Line width from G-Force’s frame size. G-Force draws frames above ~5.2 MP (e.g. Retina full screen) at half size, so lines jump thicker there.' },
      { v: 'frame', label: 'Frame size', tag: 'ok', tagText: 'reforce',
        note: 'Line width from reforce’s real frame size: consistent at every size; about 14% thinner than G-Force in Retina full screen, identical below ~5.2 MP.' },
    ] },
    { id: 'sprites', title: 'Sprites and the flow', setting: 'reforce.SpriteLayering', values: [
      { v: 'gforce', label: 'Overlay', tag: 'gf', tagText: 'G-Force',
        note: 'Like G-Force (probe #40): sprites sit on top of the picture, half-way lighten, no trails; the flow keeps running underneath. Sprites with their own Pen still stamp into the flow.' },
      { v: 'melt', label: 'Melt', tag: '', tagText: 'reforce classic',
        note: 'reforce’s original look: sprites are stamped into the flow, so they melt and stream with the FlowField and wipe the trails under them.' },
      { v: 'trails', label: 'Overlay + trails', tag: 'ok', tagText: 'reforce',
        note: 'The G-Force overlay, plus the bright parts of the image fed softly into the flow (reforce.SpriteTrails, default 0.35): crisp sprite, glowing trails, nothing wiped.' },
    ] },
  ];
  // Same as engine::lookPreset.
  const LOOK_PRESETS = {
    gforce: { thinlines: 'gforce', intensity: 'gforce', flowrounding: 'fixed16', flowprecision: 'double', aa: 'on', linescale: 'gforce', sprites: 'gforce' },
    right: { thinlines: 'smooth', intensity: 'hq', flowrounding: 'nearest', flowprecision: 'double', aa: 'on', linescale: 'frame', sprites: 'gforce' },
  };
  const lookViews = new Map();
  const lookPost = (body) => post(Object.assign({ action: 'look' }, body));
  const lookLabel = (def, v) => {
    const x = def.values.concat(def.advanced || []).find((o) => o.v === v);
    return x ? x.label : v;
  };

  function setupLook() {
    const rows = $('#lookRows');
    for (const def of LOOK) {
      const row = document.createElement('div');
      row.className = 'look-row';
      const h = document.createElement('h3');
      h.textContent = def.title;
      const sm = document.createElement('small');
      sm.textContent = def.setting;
      h.append(sm);
      const seg = document.createElement('div');
      seg.className = 'seg';
      const buttons = new Map();
      for (const o of def.values) {
        const b = document.createElement('button');
        b.className = 'toggle';
        b.setAttribute('aria-pressed', 'false');
        b.textContent = o.label;
        if (o.tagText) {
          const i = document.createElement('i');
          i.className = o.tag;
          i.textContent = o.tagText;
          b.append(i);
        }
        b.title = o.note;
        b.addEventListener('click', () => lookPost({ key: def.id, value: o.v }));
        seg.append(b);
        buttons.set(o.v, b);
      }
      let adv = null;
      if (def.advanced) {
        adv = document.createElement('select');
        adv.setAttribute('aria-label', def.title + ': other modes');
        const none = document.createElement('option');
        none.value = '';
        none.textContent = 'other…';
        adv.append(none);
        for (const o of def.advanced) {
          const opt = document.createElement('option');
          opt.value = o.v;
          opt.textContent = o.label;
          adv.append(opt);
        }
        adv.addEventListener('change', () => { if (adv.value) lookPost({ key: def.id, value: adv.value }); });
        seg.append(adv);
      }
      const note = document.createElement('p');
      note.className = 'note';
      const extra = document.createElement('p');
      extra.className = 'note';
      extra.hidden = true;
      const foot = document.createElement('div');
      foot.className = 'foot';
      const saved = document.createElement('span');
      saved.className = 'saved';
      const save = document.createElement('button');
      save.textContent = 'Save as default';
      save.addEventListener('click', () => lookPost({ op: 'save', key: def.id }));
      const reset = document.createElement('button');
      reset.textContent = 'Reset';
      reset.title = 'Back to the saved value';
      reset.addEventListener('click', () => lookPost({ op: 'reset', key: def.id }));
      foot.append(saved, save, reset);
      let rebuild = null;
      if (def.rebuild) {
        rebuild = document.createElement('button');
        rebuild.textContent = 'Rebuild field now';
        rebuild.title = 'Quick transition to the current FlowField, rebuilt with this precision';
        rebuild.addEventListener('click', () => {
          const ls = stateLayer('flowfield');
          if (ls && ls.current) post({ layer: 'flowfield', action: 'switch', config: ls.current });
        });
        foot.append(rebuild);
      }
      row.append(h, seg, note, extra, foot);
      rows.append(row);
      lookViews.set(def.id, { def, row, buttons, adv, note, extra, saved, save, reset, rebuild });
    }
    rememberOpen($('#lookWrap'), 'reforce.lookOpen');  // collapsed unless opened
    $('#lookAB').addEventListener('click', () => lookPost({ op: 'ab' }));
    $('#lookGForce').addEventListener('click', () => lookPost({ op: 'preset', value: 'gforce' }));
    $('#lookRight').addEventListener('click', () => lookPost({ op: 'preset', value: 'right' }));
    $('#lookSaveAll').addEventListener('click', () => lookPost({ op: 'save' }));
    $('#lookResetAll').addEventListener('click', () => lookPost({ op: 'reset' }));
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'b' || e.metaKey || e.ctrlKey || e.altKey || e.repeat) return;
      const t = e.target;
      if (t && (t.isContentEditable || /^(INPUT|SELECT|TEXTAREA)$/.test(t.tagName))) return;
      if ($('#look').hidden || $('#review').open) return;
      e.preventDefault();
      lookPost({ op: 'ab' });
    });
  }

  function updateLook(l) {
    const section = $('#look');
    section.hidden = !l;
    if (!l) return;
    const chip = $('#lookSlot');
    const same = (a, b) => LOOK.every((d) => a[d.id] === b[d.id]);
    const which = same(l.current, LOOK_PRESETS.gforce) ? 'G-Force exact' : same(l.current, LOOK_PRESETS.right) ? 'Done right' : 'custom';
    const slotText = (l.slot ? `slot ${l.slot} · ` : '') + which;
    chip.textContent = slotText;
    chip.className = 'chip status' + (l.slot ? ' on' : '');
    const note = $('#lookSummary');
    note.textContent = slotText;
    note.className = 'vsum-note' + (l.slot ? ' on' : '');
    const differs = new Set(l.differs || []);
    for (const [id, v] of lookViews) {
      const cur = l.current[id];
      for (const [val, b] of v.buttons) b.setAttribute('aria-pressed', String(val === cur));
      if (v.adv) {
        const inAdv = v.def.advanced.some((o) => o.v === cur);
        v.adv.value = inAdv ? cur : '';
        v.adv.classList.toggle('active', inAdv);
      }
      const o = v.def.values.concat(v.def.advanced || []).find((x) => x.v === cur);
      v.note.textContent = o ? o.note : cur;
      let extra = '';
      if (id === 'flowrounding' && l.current.intensity === 'hq') extra = 'Has no effect while the trail buffer is 16-bit float (hq).';
      if (id === 'flowprecision') {
        extra = l.fieldPrecision && l.fieldPrecision !== cur
          ? `Applies from the next FlowField build; the field shown was built in ${l.fieldPrecision}.`
          : 'Applies from the next FlowField build (switch or slideshow).';
      }
      v.extra.textContent = extra;
      v.extra.hidden = !extra;
      if (v.rebuild) v.rebuild.hidden = !(l.fieldPrecision && l.fieldPrecision !== cur);
      const d = differs.has(id);
      v.row.classList.toggle('differs', d);
      v.saved.textContent = d ? `differs from saved (${lookLabel(v.def, l.saved[id])})` : 'same as saved';
      v.save.disabled = !d;
      v.reset.disabled = !d;
    }
    $('#lookSaveAll').disabled = differs.size === 0;
    $('#lookResetAll').disabled = differs.size === 0;
  }

  // ------------------------------------------------------------ rhythm (PLAN 5O)
  // Phrase strip, phrase-start buttons, quantize choices and kick zoom; commands go out as beatset.
  const bset = (key, value) => post({ action: 'beatset', key, value: value === undefined ? '' : value });
  const rhEditing = {};
  // An on/off toggle button sent as beatset (shown at once; the next full state confirms it).
  function beatToggle(el, key) {
    el.addEventListener('click', () => {
      const on = el.getAttribute('aria-pressed') !== 'true';
      el.setAttribute('aria-pressed', String(on));
      bset(key, on ? 'on' : 'off');
    });
  }
  // A Rhythm / Dancers slider: its <b> label follows while dragging; sent (beatset, saved) on release. State updates
  // leave it alone for 1.5 s after it was touched (beatSliderShow).
  function beatSlider(el, labelId, key, fmt) {
    const mark = () => { rhEditing[el.id] = Date.now() + 1500; };
    el.addEventListener('pointerdown', mark);
    el.addEventListener('input', () => { mark(); setText($('#' + labelId), fmt(Number(el.value))); });
    el.addEventListener('change', () => { mark(); bset(key, el.value); });
    el.dataset.fmt = labelId;
    el._fmt = fmt;
  }
  function beatSliderShow(id, v) {
    const el = $('#' + id);
    if (!el || v === undefined || (rhEditing[id] && Date.now() < rhEditing[id])) return;
    el.value = String(v);
    setText($('#' + el.dataset.fmt), el._fmt(Number(v)));
  }
  function setupRhythm() {
    $('#rhNow').addEventListener('click', () => bset('phrasenow'));
    $('#rhEarlier').addEventListener('click', () => bset('phrasenudge', '-1'));
    $('#rhLater').addEventListener('click', () => bset('phrasenudge', '1'));
    beatToggle($('#rhAuto'), 'phraseauto');
    $('#rhBars').addEventListener('change', (e) => bset('phrasebars', e.target.value));
    beatToggle($('#rhDecks'), 'phrasedecks');
    beatToggle($('#rhNp'), 'nowplaying');
    $('#rhNpCorner').addEventListener('change', (e) => bset('nowplayingcorner', e.target.value));
    beatToggle($('#rhDropSwitch'), 'dropswitch');
    for (const [id, key] of [['rhQuantize', 'quantize'], ['rhQuantizeManual', 'quantizemanual'],
                             ['rhQuantizeSpawn', 'quantizespawn']])
      $('#' + id).addEventListener('change', (e) => bset(key, e.target.value));
    const sliders = [
      ['rhZoom', 'rhZoomVal', 'kickzoom', (v) => (v * 100).toFixed(2) + ' %'],
      ['rhRot', 'rhRotVal', 'kickrotate', (v) => v.toFixed(2) + '°'],
      ['rhDecay', 'rhDecayVal', 'kickdecay', (v) => v.toFixed(0) + ' ms'],
      ['rhNpSize', 'rhNpSizeVal', 'nowplayingsize', (v) => v.toFixed(0) + ' %'],
      ['rhNpSwitch', 'rhNpSwitchVal', 'nowplayingswitch', (v) => v.toFixed(1).replace(/\.0$/, '') + ' s'],
      ['rhNpShadow', 'rhNpShadowVal', 'nowplayingshadow', (v) => Math.round(v * 100) + ' %'],
      ['rhNpShadowFrom', 'rhNpShadowFromVal', 'nowplayingshadowfrom', (v) => v.toFixed(2)],
    ];
    for (const [id, lbl, key, fmt] of sliders) beatSlider($('#' + id), lbl, key, fmt);
  }
  // ------------------------------------------------------------ dancers (PLAN 5P)
  // Floor, crew mode, lighting, size and outline go out as beatset (saved to settings.json); the live list shows who
  // is dancing what.
  // The dance styles (engine/DanceMoves Style; the mocap genres, Peter 2026-11-21), in order.
  // [key, label, the "Random <Style>" dancer file that makes a new dancer of that style].
  const DANCE_STYLES = [['house', 'house', 'Random House'], ['pop', 'pop', 'Random Pop'], ['lock', 'lock', 'Random Lock'],
    ['break', 'break', 'Random Break'], ['disco', 'disco', 'Random Disco'], ['stomp', 'stomp', 'Random Stomp'],
    ['hiphop', 'hip-hop', 'Random Hip-Hop'], ['jazz', 'jazz', 'Random Jazz'], ['ballet', 'ballet', 'Random Ballet']];
  function setupDancerControls(after) {
    const box = document.createElement('div');
    box.className = 'dance';
    box.innerHTML = `
      <div class="row buttons dancerow">
        <button class="toggle" id="dnHide" aria-pressed="false" title="Hide the dancers for now (fades them out). They keep dancing behind the scenes: guests keep their time and new ones still arrive, so showing them again brings back everyone still due. Not saved: a restart shows them.">Hide dancers</button>
      </div>
      <div class="row buttons dancerow">
        <span class="dn-guest-status" id="dnGuestStatus"></span>
      </div>
      <div class="rh-grid">
        <label class="field" title="How many generated dancers may be on stage at once (1-32). Stream guests don't count: they come and go regardless. A new one sends the oldest generated dancer off; lowering it sends the oldest off waving. With a crowd the camera pulls back to fit everyone. Saved to settings.json (reforce.Dancer.MaxLive).">
          <span>Max dancers <b id="dnMaxVal">5</b></span><input type="range" id="dnMax" min="1" max="32" step="1"></label>
      </div>
      <div class="dn-random" title="Bring on a new dancer of that style: someone new each time, dancing the style's moves and dressed to match (with Max dancers on stage, the oldest leaves). Start next picks the style itself.">
        <span>New dancer</span>
        <div class="chips" id="dnNew"></div>
      </div>
      <details class="dn-more" id="dnMore">
        <summary title="Lighting, reflections, size, outline and how human the dancing looks">Look: light, reflections, size, outline, human</summary>
        <div class="rh-grid">
        <label class="field" title="How much the visuals light the dancers: 0 = a plain studio light, 1 = lit only by the colours on screen (they still never go darker than half the studio light). Saved to settings.json (reforce.Dancer.Lighting).">
          <span>Lit by visuals <b id="dnLightVal">0.75</b></span><input type="range" id="dnLight" min="0" max="1" step="0.05"></label>
        <label class="field" title="The dancers mirrored in the disco floor: 0 = none, 1 = strong; they fade with depth below the floor. Saved to settings.json (reforce.Dancer.Reflection).">
          <span>Reflections <b id="dnReflectVal">0.35</b></span><input type="range" id="dnReflect" min="0" max="1" step="0.05"></label>
        <label class="field" title="Dancer height as a share of the frame height. Saved to settings.json (reforce.Dancer.Height).">
          <span>Size <b id="dnHeightVal">24 %</b></span><input type="range" id="dnHeight" min="0.12" max="0.7" step="0.01"></label>
        <label class="field" title="Cartoon outline width in pixels (at 1080p; 0 = none). Saved to settings.json (reforce.Dancer.Outline).">
          <span>Outline <b id="dnOutlineVal">2 px</b></span><input type="range" id="dnOutline" min="0" max="5" step="0.25"></label>
        <label class="field" title="How long a stream guest dances after tapping “I'm here” (tapping again later brings them back). Saved to settings.json (reforce.Guests.Minutes).">
          <span>Guests stay <b id="dnGuestMinVal">30 min</b></span><input type="range" id="dnGuestMin" min="1" max="60" step="1"></label>
        <label class="field" title="How human the dancing looks: 0 = every dancer exactly on the grid, 1 = each dancer has their own timing, size of moves and posture, with slow drifts and follow-through, 2 = sloppy. Saved to settings.json (reforce.Dancer.Human).">
          <span>Human <b id="dnHumanVal">1.00</b></span><input type="range" id="dnHuman" min="0" max="2" step="0.05"></label>
        </div>
      </details>
      <details class="dn-more" id="dnMocap">
        <summary title="Dance moves recorded from real dancers (AIST++): try them one by one">Mocap moves</summary>
        <div class="row buttons dancerow" title="Every dancer on stage (and each new one) loops the chosen move from its start, beginning on the next bar line, until Stop. Basic: one step, 4 bars, repeated. Routine: a 16-bar choreography (or 8 bars where only half was usable). BPM: the tempo it was recorded at. Not saved.">
          <label class="field compact"><span>Style</span><select id="dnTryGenre"></select></label>
          <label class="field compact dn-try-move"><span>Move</span><select id="dnTryMove"></select></label>
        </div>
        <div class="row buttons dancerow">
          <button id="dnTryPrev" title="The previous move in this style">◀ Prev</button>
          <button id="dnTryPlay" class="toggle" aria-pressed="false" title="Everyone dances the chosen move (on: Stop goes back to their own moves)">Try</button>
          <button id="dnTryNext" title="The next move in this style">Next ▶</button>
          <span class="dn-guest-status" id="dnTryNow"></span>
        </div>
      </details>
      <details class="dn-more" id="dnPast" hidden>
        <summary title="Stream guests who have checked in before: bring one back on stage by hand, to dance until you send them off or Clear. Each guest's latest look is kept in Guests/ next to settings.json. Blocked guests are listed at the end (✕ unblocks).">Past guests <span id="dnPastCount"></span></summary>
        <ul class="dancing" id="dnPastList"></ul>
        <div class="dn-blocked" id="dnBlocked" hidden></div>
      </details>
      <ul class="dancing" id="dnList" title="Who is on stage, their dance style and the routine they are doing now"></ul>
`;
    after.after(box);
    // Who is dancing what goes in the card's collapsed "On stage" section (below the buttons, see buildExtra), so
    // a new dancer never pushes "Start next" down.
    const alive = after.closest('article') && $('.alive', after.closest('article'));
    if (alive) {
      $('.alive-list', alive).hidden = true;
      alive.append($('#dnList', box));
      // The crew mode ("cypher", "partners", ...) after the count in the section's header.
      const mode = Object.assign(document.createElement('span'), { id: 'dnMode', className: 'alive-mode' });
      $('summary', alive).append(mode);
    }
    beatToggle($('#dnHide', box), 'dancehide');
    // One button per style: a new dancer of that style (its "Random <Style>" file).
    const newRow = $('#dnNew', box);
    for (const [style, label, file] of DANCE_STYLES) {
      const b = Object.assign(document.createElement('button'), { textContent: label, title: `A new ${label} dancer` });
      b.dataset.style = style;
      b.addEventListener('click', () => post({ command: `switch:dancer:${file}` }));
      newRow.append(b);
    }
    // The look sliders stay folded away (Max dancers is the one used live).
    rememberOpen($('#dnMore', box), 'reforce.dancerMoreOpen');
    rememberOpen($('#dnMocap', box), 'reforce.dancerMocapOpen');
    rememberOpen($('#dnPast', box), 'reforce.dancerPastOpen');
    setupMoveBrowser(box);
    for (const [id, lbl, key, fmt] of [
      ['dnLight', 'dnLightVal', 'dancelight', (v) => v.toFixed(2)],
      ['dnReflect', 'dnReflectVal', 'dancereflect', (v) => v.toFixed(2)],
      ['dnHeight', 'dnHeightVal', 'danceheight', (v) => Math.round(v * 100) + ' %'],
      ['dnOutline', 'dnOutlineVal', 'danceoutline', (v) => v.toFixed(2).replace(/\.?0+$/, '') + ' px'],
      ['dnHuman', 'dnHumanVal', 'dancehuman', (v) => v.toFixed(2)],
      ['dnMax', 'dnMaxVal', 'dancemax', (v) => String(Math.round(v))],
      ['dnGuestMin', 'dnGuestMinVal', 'guestminutes', (v) => Math.round(v) + ' min'],
    ]) beatSlider($('#' + id, box), lbl, key, fmt);
  }
  // Mocap move browser: pick a style, then step through its moves; Try sends beatset dancetry (everyone loops it).
  function setupMoveBrowser(box) {
    const genreSel = $('#dnTryGenre', box), moveSel = $('#dnTryMove', box), play = $('#dnTryPlay', box);
    const moves = library.danceMoves || [];
    if (!moves.length) {
      $('#dnMocap', box).hidden = true;
      return;
    }
    for (const st of library.danceStyles || []) {
      const n = moves.filter((m) => m.style === st.key).length;
      if (n) genreSel.append(new Option(`${st.name} (${n})`, st.key));
    }
    // Within a style: basic moves first, then routines; a style made of two genres (hip-hop: party and LA) keeps the
    // genre in the label ("party hip-hop basic 3 · dancer 23").
    const fill = () => {
      const own = moves.filter((m) => m.style === genreSel.value);
      const one = new Set(own.map((m) => m.label.split(/ (basic|routine) /)[0])).size === 1;
      own.sort((a, b) => Number(a.routine) - Number(b.routine));
      moveSel.replaceChildren(...own.map((m) => new Option(
        (one ? m.label.replace(/^.*? (basic|routine) /, '$1 ') : m.label) + ` · ${m.bpm} bpm`, m.id)));
    };
    const tryNow = () => {
      play.setAttribute('aria-pressed', 'true');
      bset('dancetry', moveSel.value);
    };
    genreSel.addEventListener('change', () => {
      fill();
      if (play.getAttribute('aria-pressed') === 'true') tryNow();
    });
    moveSel.addEventListener('change', () => {
      if (play.getAttribute('aria-pressed') === 'true') tryNow();
    });
    const step = (d) => {
      const n = moveSel.options.length;
      if (!n) return;
      moveSel.selectedIndex = (moveSel.selectedIndex + d + n) % n;
      tryNow();
    };
    $('#dnTryPrev', box).addEventListener('click', () => step(-1));
    $('#dnTryNext', box).addEventListener('click', () => step(1));
    play.addEventListener('click', () => {
      if (play.getAttribute('aria-pressed') === 'true') {
        play.setAttribute('aria-pressed', 'false');
        bset('dancetry', 'off');
      } else tryNow();
    });
    genreSel.fillMoves = fill;
    fill();
  }
  function updateMoveBrowser(r) {
    const play = $('#dnTryPlay'), now = $('#dnTryNow');
    if (!play || r.danceTry === undefined) return;
    const on = !!r.danceTry;
    play.setAttribute('aria-pressed', String(on));
    play.textContent = on ? 'Stop' : 'Try';
    const m = on && (library.danceMoves || []).find((x) => x.id === r.danceTry);
    now.textContent = m ? 'trying: ' + m.label : '';
    if (m && $('#dnTryMove').value !== m.id) {
      const g = $('#dnTryGenre');
      if (g.value !== m.style) {
        g.value = m.style;
        if (g.fillMoves) g.fillMoves();
      }
      $('#dnTryMove').value = m.id;
    }
  }
  // Past guests: one row each (name, style, when last seen) with Bring on, or "on stage" while they dance.
  const STYLE_LABEL = Object.fromEntries(DANCE_STYLES);
  const OLD_STYLE = { robot: 'pop', rave: 'house', retro: 'lock', chill: 'jazz' };
  function updatePastGuests(r) {
    const box = $('#dnPast'), list = $('#dnPastList');
    if (!box || !r.guestBook) return;
    // Shown when there is anyone to bring back or anyone blocked (blocked guests are listed inside it).
    box.hidden = !r.guestBook.length && !(r.guestsBlocked || []).length;
    const onStage = new Set((r.danceRuns || []).filter((d) => d.guest && !d.leaving).map((d) => d.guest));
    const sig = r.guestBook.map((g) => `${g.id}=${g.label}:${g.style}:${g.seen}:${onStage.has(g.id) ? 1 : 0}`).join('|');
    if (list.dataset.sig === sig) return;
    list.dataset.sig = sig;
    setText($('#dnPastCount'), r.guestBook.length ? `(${r.guestBook.length})` : '');
    const when = (ms) => {
      if (!ms) return '';
      const d = new Date(ms);
      return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' }) + ' ' +
        d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
    };
    list.replaceChildren(...r.guestBook.map((g) => {
      const li = document.createElement('li');
      li.className = 'dn-run';
      const style = OLD_STYLE[g.style] || g.style;
      li.append(Object.assign(document.createElement('span'), { className: 'dn-what',
        textContent: `${g.label} (${STYLE_LABEL[style] || style || '?'}) · seen ${when(g.seen)}` }));
      if (onStage.has(g.id)) {
        li.append(Object.assign(document.createElement('span'), { className: 'muted', textContent: 'on stage' }));
      } else {
        const on = Object.assign(document.createElement('button'), { className: 'mini', textContent: 'Bring on',
          title: `Put ${g.label} on stage, dancing until you send them off or Clear` });
        on.addEventListener('click', () => bset('guestcall', g.id));
        li.append(on);
      }
      return li;
    }));
  }
  function updateDancers(r, full) {
    updatePastGuests(r);
    const list = $('#dnList');
    if (!list) return;
    if (r.danceMode !== undefined) setText($('#dnMode'), r.danceMode ? ` · ${r.danceMode}` : '');
    if (r.dancing) {
      // One row per dancer: what they are doing, Send off, and Block for stream guests. Rows are rebuilt only when
      // who is on stage changes (the text updates in place, so a click is never lost to a re-render).
      const runs = r.danceRuns || r.dancing.map((d, i) => ({ id: -1 - i, name: d, guest: '', leaving: false }));
      const sig = runs.map((d) => `${d.id}:${d.leaving ? 1 : 0}`).join('|');
      if (list.dataset.sig !== sig) {
        list.dataset.sig = sig;
        list.replaceChildren(...runs.map((d) => {
          const li = document.createElement('li');
          li.className = 'dn-run';
          li.append(Object.assign(document.createElement('span'), { className: 'dn-what' }));
          if (d.id >= 0 && !d.leaving) {
            const off = Object.assign(document.createElement('button'), { className: 'mini', textContent: 'Send off',
              title: 'Wave goodbye and leave the stage now' });
            off.addEventListener('click', () => bset('dancerleave', String(d.id)));
            li.append(off);
            if (d.guest) {
              const block = Object.assign(document.createElement('button'), { className: 'mini', textContent: 'Block',
                title: `Send ${d.name} off and never put this guest on stage again (Unblock below). Saved to settings.json (reforce.Guests.Blocked).` });
              block.addEventListener('click', () => {
                const ids = new Set((list.dataset.blocked || '').split(',').filter(Boolean));
                ids.add(d.guest);
                bset('guestsblocked', [...ids].join(',') || 'none');
              });
              li.append(block);
            }
          }
          return li;
        }));
      }
      [...list.children].forEach((li, i) => setText($('.dn-what', li), r.dancing[i] || ''));
    }
    if (r.guestsBlocked) {
      const box = $('#dnBlocked');
      const ids = r.guestsBlocked.map((g) => g.id);
      list.dataset.blocked = ids.join(',');
      const sig = r.guestsBlocked.map((g) => g.id + '=' + g.name).join('|');
      if (box && box.dataset.sig !== sig) {
        box.dataset.sig = sig;
        box.hidden = !ids.length;
        box.replaceChildren(Object.assign(document.createElement('span'), { className: 'muted', textContent: 'Blocked guests: ' }),
          ...r.guestsBlocked.map((g) => {
            const b = Object.assign(document.createElement('button'), { className: 'mini',
              textContent: `${g.name || g.id} ✕`, title: 'Unblock: this guest can join the floor again' });
            b.addEventListener('click', () => bset('guestsblocked', ids.filter((x) => x !== g.id).join(',') || 'none'));
            return b;
          }));
      }
    }
    if (!full) return;
    $('#dnHide').setAttribute('aria-pressed', String(!!r.danceHidden));
    setText($('#dnGuestStatus'), !r.guests ? 'Stream guests off' : !r.guestStatus ? 'Stream guests: connecting…'
      : r.guestStatus === 'ok' ? `Stream guests: ${r.guestArrivals || 0} check-in${r.guestArrivals === 1 ? '' : 's'} this session`
      : `Stream guests: ⚠ ${r.guestStatus}`);
    for (const [id, v] of [['dnLight', r.danceLighting], ['dnReflect', r.danceReflection], ['dnHeight', r.danceHeight],
                           ['dnOutline', r.danceOutline], ['dnHuman', r.danceHuman], ['dnMax', r.danceMax], ['dnGuestMin', r.guestMinutes]])
      beatSliderShow(id, v);
  }
  function updateRhythm(r, full) {
    if (!r) return;
    updateDancers(r, full);
    updateMoveBrowser(r);
    const strip = $('#rhPhrase');
    const bars = Math.max(1, Math.round(r.bars || 16));
    if (strip.children.length !== bars) {
      strip.textContent = '';
      for (let k = 0; k < bars; ++k) strip.append(document.createElement('i'));
      strip.style.setProperty('--bars', bars);
    }
    const on = !!r.on;
    strip.classList.toggle('off', !on);
    const now = on ? Math.max(0, Math.min(bars - 1, r.barInPhrase | 0)) : -1;
    const cells = strip.children;
    for (let k = 0; k < cells.length; ++k) {
      cells[k].classList.toggle('now', k === now);
      cells[k].classList.toggle('done', on && k < now);
    }
    setScale($('#rhKick'), 'X', r.kick || 0);
    setScale($('#rhBuild'), 'X', r.breakdown ? r.build || 0 : r.drop || 0);
    const dt = $('#rhDropText');
    const hit = (r.drop || 0) > 0.3;
    dt.classList.toggle('hit', hit);
    setText(dt, hit ? `drop! (${r.drops || 0} so far)` : r.breakdown ? `breakdown · build ${Math.round((r.build || 0) * 100)} %`
      : (r.drops ? `${r.drops} drop${r.drops === 1 ? '' : 's'} so far` : 'waiting for a breakdown'));
    if (r.nowPlayingLine !== undefined) setText($('#rhNpLine'), r.nowPlayingLine || 'nothing yet (needs rkbx_link)');
    if (r.deckInputs !== undefined) {
      // Decks, then each input coloured by whether it is there (missing ones start with "no ").
      const el = $('#rhDeckLine');
      const key = (r.decks || '') + '|' + (r.deckInputs || '');
      if (el.dataset.key !== key) {
        el.dataset.key = key;
        el.textContent = '';
        if (r.deckInputs) {
          if (r.decks) el.append(Object.assign(document.createElement('span'), { className: 'decks', textContent: r.decks }));
          for (const part of r.deckInputs.split(' · ')) {
            const s = document.createElement('span');
            s.className = part.startsWith('no ') ? 'in bad' : 'in ok';
            s.textContent = part;
            el.append(s);
          }
        }
      }
    }
    if (r.how !== undefined || full) {
      const how = r.how || 'none';
      const conf = Number(r.confidence || 0);
      const phraseNo = on ? Math.floor(Number(r.phrase || 0)) : 0;
      setText($('#rhStatus'), !on ? 'no beat clock'
        : how === 'auto' ? `auto · ${(conf * 100).toFixed(0)} % sure · phrase ${phraseNo}`
        : how === 'manual' ? `set by hand · phrase ${phraseNo}`
        : how === 'deck' ? `deck ${(r.deck ?? -1) + 1} start · phrase ${phraseNo}`
        : `guessing (Link bar 1) · phrase ${phraseNo}`);
    }
    if (!full) return;
    $('#rhAuto').setAttribute('aria-pressed', String(!!r.autoAlign));
    $('#rhDecks').setAttribute('aria-pressed', String(r.deckStarts !== false));
    $('#rhDropSwitch').setAttribute('aria-pressed', String(!!r.dropSwitch));
    $('#rhNp').setAttribute('aria-pressed', String(!!r.nowPlaying));
    const sel = (id, v) => { const el = $('#' + id); if (document.activeElement !== el && v !== undefined) el.value = String(v); };
    sel('rhBars', bars);
    sel('rhQuantize', r.quantize);
    sel('rhQuantizeManual', r.quantizeManual);
    sel('rhQuantizeSpawn', r.quantizeSpawn);
    sel('rhNpCorner', r.nowPlayingCorner);
    for (const [id, v] of [['rhZoom', r.kickZoom], ['rhRot', r.kickRotate], ['rhDecay', r.kickDecayMs],
                           ['rhNpSize', r.nowPlayingSize], ['rhNpSwitch', r.nowPlayingSwitch],
                           ['rhNpShadow', r.nowPlayingShadow], ['rhNpShadowFrom', r.nowPlayingShadowFrom]])
      beatSliderShow(id, v);
  }

  // ------------------------------------------------------------ state
  function apply(s) {
    state = s;
    stateAt = performance.now();
    updateGlobal(s);
    updateVideo(s.video);
    updateLook(s.look);
    updateRhythm(s.rhythm, true);
    for (const key of MAIN) updateLayer(views.get(key), stateLayer(key));
    for (const key of EXTRA) {
      const v = views.get(key);
      if (v) updateExtra(v, stateLayer(layerOf(key)));
    }
    tickCountdowns();
  }

  // SSE `live` event (20 Hz between full states): meters, Link, progress, countdowns.
  function applyLive(lv) {
    if (!state) return;
    stateAt = performance.now();
    state.simTime = lv.simTime;
    state.fps = lv.fps;
    if (lv.audio) state.audio = lv.audio;
    for (const l of lv.layers || []) {
      const ls = stateLayer(l.key);
      if (ls) Object.assign(ls, l);
    }
    if (lv.video && state.video) state.video.crossfade = lv.video.crossfade;
    if (lv.video && lv.video.lights && state.video && state.video.lights) {
      Object.assign(state.video.lights, lv.video.lights);
      drawLightsPreview(state.video.lights);
    }
    if (lv.rhythm && state.rhythm) {
      Object.assign(state.rhythm, lv.rhythm);
      updateRhythm(state.rhythm, false);
    }
    updateMeters(state.audio, false);
    updateStats(state);
    for (const key of MAIN) updateProgress(views.get(key), stateLayer(key));
    for (const key of EXTRA) {
      const v = views.get(key);
      if (v) extraCountdown(v, stateLayer(layerOf(key)));
    }
    const vf = state.video ? state.video.crossfade : -1;
    setScale($('#vFade'), 'X', vf >= 0 ? vf : 0);
    tickCountdowns();
  }

  let es = null;
  // Live update rate (Hz; 0 = full states only, every 2 s or on a change), per browser.
  const liveRate = () => {
    const v = Number(store.get('reforce.liveRate'));
    return [0, 5, 10, 20].includes(v) && store.get('reforce.liveRate') !== null ? v : 10;
  };
  function setupLiveRate() {
    const sel = $('#liveRate');
    if (!sel) return;
    sel.value = String(liveRate());
    sel.addEventListener('change', () => {
      store.set('reforce.liveRate', sel.value);
      if (es) { es.close(); es = null; }
      if (!document.hidden) connect();
    });
  }

  function connect() {
    const dot = $('#conn');
    if (!window.EventSource) {
      // Fallback: poll.
      setInterval(async () => {
        try {
          const r = await fetch(api('/api/state'));
          if (r.ok) { apply(await r.json()); dot.className = 'dot ok'; } else dot.className = 'dot bad';
        } catch { dot.className = 'dot bad'; }
      }, 500);
      return;
    }
    if (es) return;
    es = new EventSource(api(`/api/events?live=${liveRate()}`));
    es.onopen = () => { dot.className = 'dot ok'; dot.title = 'connected'; };
    es.onerror = () => { dot.className = 'dot bad'; dot.title = 'reconnecting…'; };
    es.onmessage = (e) => {
      try { apply(JSON.parse(e.data)); } catch { /* ignore a bad frame */ }
    };
    es.addEventListener('live', (e) => {
      try { applyLive(JSON.parse(e.data)); } catch { /* ignore a bad frame */ }
    });
  }

  // A hidden tab (another tab in front, window minimized) stops the stream: no CPU
  // here or in reforce. Showing it again reconnects; the first event is a full state.
  document.addEventListener('visibilitychange', () => {
    if (!window.EventSource) return;
    if (document.hidden) {
      if (es) { es.close(); es = null; }
      clearTimeout(linkTimer);
      const dot = $('#conn');
      dot.className = 'dot';
      dot.title = 'paused (tab hidden)';
    } else {
      connect();
    }
  });

  async function init() {
    setupGlobal();
    setupLiveRate();
    let first = null;
    try {
      const [lr, sr] = await Promise.all([fetch(api('/api/library')), fetch(api('/api/state'))]);
      if (lr.status === 401 || sr.status === 401) {
        toast('Access denied: open the URL with ?token=… from the reforce log or window title.');
        $('#conn').className = 'dot bad';
        return;
      }
      if (lr.ok) library = await lr.json();
      if (sr.ok) first = await sr.json();
    } catch {
      toast('Cannot reach reforce');
    }
    library.layers = library.layers || [];
    library.themes = library.themes || [];

    const layers = $('#layers');
    for (const key of MAIN) layers.append(buildLayer(key));
    const extraGrid = $('#extraGrid');
    for (const key of EXTRA) {
      const cfgs = libLayer(layerOf(key)).configs;
      const hasLib = key === 'movie' ? cfgs.some((c) => c.movie) : cfgs.length > 0;
      const hasState = first && first.layers && first.layers.some((l) => l.key === layerOf(key));
      if (!hasLib && !hasState) continue;
      const card = buildExtra(key);
      if (key === 'union') {
        // A Union shapes FlowField transitions: it lives at the foot of the FlowField card.
        card.classList.add('union-sub');
        views.get('flowfield').root.append(card);
      } else extraGrid.append(card);
    }
    $('#extras').hidden = !extraGrid.children.length;
    setupTabs();
    setupScrollIntent();
    $('#favClear').addEventListener('click', () => post({ command: 'kill:sprite' }));
    setupReview();
    Promise.all([loadFavorites(), loadHidden()]).then(() => {
      renderFavorites();
      for (const v of views.values()) if (v.list) renderList(v);
      updateReviewButton();
    });
    setupVideo();
    setupLook();
    setupRhythm();
    if (first) apply(first);
    connect();
    setInterval(tickCountdowns, 250);
  }

  init();
})();
