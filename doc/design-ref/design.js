
class Component extends DCLogic {
  state = { view: 'grid', q: '', cat: 'All', sel: null, selAc: 0, selShip: 0, captured: false, now: Date.now(), fft: null, peak: null, sqlOpen: false, sm: -70, vu: 0, rxk: 98500 };
  wfRef = React.createRef();
  apRef = React.createRef();
  acs = [
    { cs: 'JBU604', ic: 'A4E8B9', alt: 39000, gs: 471, hd: 88, x: 46, y: 16, dt: 4 },
    { cs: 'UAL2413', ic: 'A1B2C3', alt: 37000, gs: 458, hd: 274, x: 33, y: 24, dt: 2 },
    { cs: 'AFR1680', ic: '39B415', alt: 34000, gs: 449, hd: 268, x: 63, y: 71, dt: 9 },
    { cs: 'DLH441', ic: '3C6589', alt: 24025, gs: 391, hd: 91, x: 58, y: 44, dt: 1 },
    { cs: 'SWA1182', ic: 'A7D3C4', alt: 12800, gs: 284, hd: 156, x: 41, y: 55, dt: 3 },
    { cs: 'RYR81QK', ic: '4CA2D1', alt: 11250, gs: 322, hd: 201, x: 70, y: 32, dt: 0 },
    { cs: '', ic: '7C4A2F', alt: 8900, gs: null, hd: null, x: null, y: null, dt: 12 },
    { cs: 'N172SP', ic: 'A9F1E2', alt: 5500, gs: 118, hd: 12, x: 23, y: 63, dt: 7 }
  ];
  ships = [
    { nm: 'PACIFIC HARMONY', mm: '366891000', sog: 18.1, cog: 78, ty: 'TANKER', x: 55, y: 22, dt: 1 },
    { nm: 'ISLAND FERRY 3', mm: '316044120', sog: 14.9, cog: 301, ty: 'PASSENGER', x: 47, y: 58, dt: 0 },
    { nm: 'NORDIC AURORA', mm: '257123000', sog: 12.4, cog: 214, ty: 'CARGO', x: 30, y: 32, dt: 3 },
    { nm: 'SEA WOLF', mm: '368224000', sog: 6.2, cog: 145, ty: 'TUG', x: 66, y: 70, dt: 6 },
    { nm: 'WESTWARD', mm: '244660321', sog: 5.8, cog: 255, ty: 'SAILING', x: 74, y: 44, dt: 9 },
    { nm: '', mm: '503112800', sog: 0.1, cog: 0, ty: '', x: 26, y: 68, dt: 14 }
  ];
  logs = [];
  pocsagRows = [];
  reconRows = [
    { f: '446.006250', mode: 'NFM', hits: 42, db: -58, ts: 0 },
    { f: '433.920000', mode: 'AM', hits: 156, db: -47, ts: 0 },
    { f: '162.550000', mode: 'NFM', hits: 89, db: -62, ts: 0 },
    { f: '121.500000', mode: 'AM', hits: 3, db: -81, ts: 0 },
    { f: '446.156250', mode: 'NFM', hits: 17, db: -70, ts: 0 },
    { f: '156.800000', mode: 'NFM', hits: 64, db: -55, ts: 0 },
    { f: '462.562500', mode: 'NFM', hits: 28, db: -66, ts: 0 }
  ];
  D = [
    ['01', 'RECEIVE', 'rx', [['ADS-B', 'adsb', 'nl', 'adsb'], ['ACARS', 'acars', 'n', 'console'], ['AIS Boats', 'ais', 'n', 'ais'], ['AFSK RX', 'rx'], ['Analog TV', 'sstv'], ['APRS RX', 'aprs'], ['Audio', 'audio', 'n', 'receiver'], ['BLE RX', 'ble'], ['ERT Meters', 'rx'], ['FSK RX', 'rx'], ['Level', 'utilities'], ['Looking Glass', 'spectrum', 'n', 'spectrum'], ['NOAA APT', 'sstv', 'n', 'image'], ['NRF RX', 'ble'], ['POCSAG', 'pocsag', 'n', 'table'], ['Radiosonde', 'sonde', 'n', 'sonde'], ['Recon', 'recon', 'n', 'table'], ['SubGHz', 'waterfall'], ['TPMS', 'rx'], ['Weather', 'rx']]],
    ['02', 'TRANSMIT', 'tx', [['ADS-B TX', 'adsb', 't'], ['APRS TX', 'aprs', 't'], ['BLE TX', 'ble', 't'], ['Burger Pager', 'pocsag', 't'], ['CW TX', 'morse', 't'], ['DTMF TX', 'tx', 't'], ['Flipper RF', 'tx', 't'], ['GPS Sim', 'gps', 't'], ['Jammer', 'tx', 't'], ['Keyfob', 'tx', 't'], ['LGE Tool', 'tx', 't'], ['Mic TX', 'mic', 't'], ['Morse TX', 'morse', 't'], ['Numbers', 'tx', 't'], ['OOK TX', 'tx', 't'], ['OOK Editor', 'tx', 't'], ['Playlist', 'tx', 't'], ['POCSAG TX', 'pocsag', 't'], ['RDS', 'audio', 't'], ['Remote IR', 'tx', 't'], ['Replay', 'capture', 't'], ['RSSI TX', 'tx', 't'], ['Signal Gen', 'siggen', 't'], ['Soundboard', 'audio', 't'], ['Spectrum Painter', 'spectrum', 't'], ['SSTV TX', 'sstv', 't'], ['TEDI', 'tx', 't'], ['TouchTunes', 'tx', 't']]],
    ['03', 'CAPTURE', 'capture', [['Capture', 'capture'], ['Raw IQ', 'capture'], ['Audio Rec', 'mic']]],
    ['04', 'SEARCH', 'search', [['Search', 'search'], ['Scanner', 'search'], ['Detector', 'search'], ['Wardrive Map', 'gps'], ['Freq Hunt', 'search']]],
    ['05', 'UTILITIES', 'utilities', [['Antenna Length', 'utilities'], ['Battery Info', 'utilities'], ['Calculator', 'utilities'], ['Clock', 'utilities'], ['Converter', 'utilities'], ['File Manager', 'acars'], ['Flash Utility', 'utilities'], ['Freq Manager', 'utilities'], ['GPS Info', 'gps'], ['Hex Editor', 'acars'], ['IQ Trim', 'siggen'], ['Notepad', 'acars'], ['SD over USB', 'capture'], ['Signal Info', 'siggen'], ['Wav Viewer', 'waterfall'], ['Whip Calc', 'utilities'], ['Screenshot', 'sstv'], ['Backlight', 'utilities']]],
    ['06', 'GAMES', 'games', [['Snake', 'games'], ['Tetris', 'games'], ['Breakout', 'games'], ['Invaders', 'games'], ['Doom', 'games'], ['Blackjack', 'games'], ['Mines', 'games']]],
    ['07', 'SETTINGS', 'settings', [['Settings', 'settings'], ['Radio', 'settings'], ['Interface', 'settings'], ['Audio Out', 'audio'], ['Bindings', 'settings'], ['Calibration', 'settings'], ['Date & Time', 'utilities'], ['Debug', 'settings'], ['Display', 'settings'], ['P.Memory', 'settings'], ['Region', 'settings'], ['Encoder', 'settings'], ['About', 'settings']]]
  ];
  componentDidMount() {
    import('./icons.js').then((m) => { this.icons = m.makeIcons(React); this.forceUpdate(); });
    const t0 = Date.now();
    this.t0 = t0;
    this.acs.forEach((a) => { a.ts = t0 - a.dt * 1000; });
    this.ships.forEach((a) => { a.ts = t0 - a.dt * 1000; });
    this.reconRows.forEach((r) => { r.ts = t0 - (5 + Math.random() * 40) * 1000; });
    this.seedPocsag(t0);
    this.clockT = setInterval(() => {
      const v = this.state.view;
      if (v === 'adsb' && Math.random() < 0.75) {
        const live = this.acs.filter((a) => a.x != null);
        live[(Math.random() * live.length) | 0].ts = Date.now();
      }
      if (v === 'ais' && Math.random() < 0.55) this.ships[(Math.random() * this.ships.length) | 0].ts = Date.now();
      this.setState({ now: Date.now() });
    }, 1000);
  }
  componentWillUnmount() { clearInterval(this.clockT); clearInterval(this.fftT); clearInterval(this.feedT); }
  tmStr(t) { return new Date(t).toISOString().slice(11, 19); }
  seedPocsag(t0) {
    const msgs = [['1204788', '3', 'ALPHA', 'UNIT 12 RESPOND MAIN ST BOX 4415'], ['0446120', '0', 'TONE', ''], ['1731055', '3', 'ALPHA', 'CALL DISPATCH 401'], ['0912664', '1', 'NUMERIC', '509 4415 22'], ['1204788', '3', 'ALPHA', 'PAGE GRP 4: SHIFT CHANGE 15:00'], ['1550023', '3', 'ALPHA', 'SYSTEM TEST - NO ACTION REQUIRED'], ['0446121', '2', 'NUMERIC', '911 0033'], ['1731055', '3', 'ALPHA', 'BATT LOW SITE 7 CHECK CHARGER'], ['0777210', '0', 'TONE', ''], ['1204790', '3', 'ALPHA', 'WATER MAIN CREW TO NE 45TH'], ['0912664', '3', 'ALPHA', 'MEET AT BAY 2 - K'], ['1550030', '1', 'NUMERIC', '206 555 0142']];
    this.pocsagRows = msgs.map((m, i) => ({ tm: this.tmStr(t0 - (i * 47 + 8) * 1000), addr: m[0], fn: m[1], ty: m[2], msg: m[3], fresh: false }));
  }
  startFft() {
    clearInterval(this.fftT); this.ph = 0;
    this.fftT = setInterval(() => {
      this.ph += 0.13;
      const N = 128, f = new Array(N);
      const carrier = 46 + Math.sin(this.ph) * 9;
      for (let i = 0; i < N; i++) {
        let db = -87 + Math.random() * 5;
        db += carrier * Math.exp(-Math.pow(i - 64, 2) / 13.5);
        db += 17 * Math.exp(-Math.pow(i - 97, 2) / 40);
        db += 12 * Math.exp(-Math.pow(i - 30, 2) / 5.8) * (Math.sin(this.ph * 2.7 + 1) > 0.2 ? 1 : 0.1);
        f[i] = Math.min(db, -23);
      }
      const p = this.state.peak ? this.state.peak.map((v, i) => Math.max(f[i], v - 0.4)) : f.slice();
      this.setState({ fft: f, peak: p, sqlOpen: carrier > 46 }, () => this.paintWf());
    }, 130);
  }
  paintWf() {
    const c = this.wfRef.current; if (!c || !this.state.fft) return;
    const ctx = c.getContext('2d');
    ctx.drawImage(c, 0, 2);
    const f = this.state.fft, w = c.width / f.length;
    for (let i = 0; i < f.length; i++) {
      ctx.fillStyle = this.wfColor((f[i] + 95) / 70);
      ctx.fillRect(i * w, 0, Math.ceil(w), 2);
    }
  }
  wfColor(v) {
    v = Math.max(0, Math.min(1, v));
    const st = [[0, [13, 12, 10]], [0.38, [16, 64, 44]], [0.7, [61, 245, 140]], [0.88, [255, 148, 56]], [1, [255, 242, 220]]];
    for (let i = 1; i < st.length; i++) if (v <= st[i][0]) {
      const a = st[i - 1], b = st[i], t = (v - a[0]) / (b[0] - a[0]);
      return 'rgb(' + a[1].map((x, j) => Math.round(x + (b[1][j] - x) * t)).join(',') + ')';
    }
    return 'rgb(255,242,220)';
  }
  startRx() {
    clearInterval(this.feedT); this.rxPh = 0;
    this.feedT = setInterval(() => {
      this.rxPh += 0.12;
      const sm = -52 + 9 * Math.sin(this.rxPh * 0.9) + Math.random() * 4;
      const open = sm > -60;
      const vu = open ? 28 + 42 * Math.abs(Math.sin(this.rxPh * 2.3)) + Math.random() * 18 : 2 + Math.random() * 3;
      this.setState({ sm, vu: Math.min(100, vu), sqlOpen: open });
    }, 120);
  }
  startConsole() {
    clearInterval(this.feedT);
    if (!this.logs.length) {
      const seed = [['AF1680', 'H1', '\u2193', 'POS N47.38 W121.55 FL340 M.79 OAT-54'], ['UAL2413', '10', '\u2193', 'ETA UPDATE KSEA 1642Z GATE B12'], ['DLH441', 'Q0', '\u2191', 'LINK TEST OK'], ['JBU604', '5Z', '\u2193', '#DFB OFF RPT 0231Z FUEL 18.4 FOB'], ['SWA1182', '80', '\u2193', 'WX REQ KSEA METAR TAF'], ['RYR81QK', 'H1', '\u2193', 'ADS-C CONTRACT ESTABLISHED'], ['N172SP', 'Q0', '\u2191', 'VOICE CONTACT REQ 131.550'], ['UAL2413', '33', '\u2193', 'FREE TEXT: CATERING SHORT 12 MEALS']];
      this.logs = seed.map((s, i) => ({ tm: this.tmStr(Date.now() - (i * 13 + 5) * 1000), fl: s[0], lab: s[1], dir: s[2], txt: s[3], fresh: false }));
    }
    const pool = [['AF1680', 'H1', '\u2193', 'POS N47.52 W121.80 FL340 M.79'], ['JBU604', '5Z', '\u2193', 'OOOI: IN 1358Z BLOCK 0412'], ['DLH441', '10', '\u2193', 'ETA UPDATE EDDF 0655Z'], ['SWA1182', 'Q0', '\u2191', 'LINK TEST OK'], ['UAL2413', '80', '\u2193', 'WX KSEA 250114Z 18008KT 10SM FEW045'], ['RYR81QK', '33', '\u2193', 'FREE TEXT: REQ STAND CHANGE'], ['N172SP', 'H1', '\u2193', 'POS N47.61 W122.10 A055']];
      this.feedT = setInterval(() => {
      const p = pool[(Math.random() * pool.length) | 0];
      this.logs.forEach((l) => { l.fresh = false; });
      this.logs.unshift({ tm: this.tmStr(Date.now()), fl: p[0], lab: p[1], dir: p[2], txt: p[3], fresh: true });
      if (this.logs.length > 40) this.logs.pop();
      this.setState({ now: Date.now() });
    }, 2100);
  }
  startTable(kind) {
    clearInterval(this.feedT);
    if (kind === 'Recon') {
      this.feedT = setInterval(() => {
        const r = this.reconRows[(Math.random() * this.reconRows.length) | 0];
        r.hits += 1; r.ts = Date.now(); r.db = -45 - Math.round(Math.random() * 35);
        this.reconRows.forEach((x) => { x.fresh = x === r; });
        this.setState({ now: Date.now() });
      }, 1700);
    } else {
      const pool = [['1204788', '3', 'ALPHA', 'UNIT 7 CLEAR SCENE'], ['0446120', '0', 'TONE', ''], ['0912664', '1', 'NUMERIC', '425 555 0107'], ['1550023', '3', 'ALPHA', 'GRP 2 STANDBY WEATHER HOLD'], ['1731055', '3', 'ALPHA', 'CALL OPS EXT 44']];
      this.feedT = setInterval(() => {
        const p = pool[(Math.random() * pool.length) | 0];
        this.pocsagRows.forEach((r) => { r.fresh = false; });
        this.pocsagRows.unshift({ tm: this.tmStr(Date.now()), addr: p[0], fn: p[1], ty: p[2], msg: p[3], fresh: true });
        if (this.pocsagRows.length > 40) this.pocsagRows.pop();
        this.setState({ now: Date.now() });
      }, 2600);
    }
  }
  paintApt() {
    const c = this.apRef.current; if (!c || c.dataset.done) return;
    c.dataset.done = '1';
    const ctx = c.getContext('2d'), W = 720, H = 440, dec = 273;
    ctx.fillStyle = '#0b0a08'; ctx.fillRect(0, 0, W, H);
    const chan = (x0, ir) => {
      for (let y = 0; y < dec; y += 2) {
        for (let x = 0; x < 268; x += 4) {
          let v = 132 + 62 * Math.sin(x * 0.016 + y * 0.021) * Math.sin(x * 0.006 - y * 0.012 + 1.7) + 34 * Math.sin(y * 0.05 + x * 0.002) + (Math.random() * 26 - 13);
          if (ir) v = 235 - v * 0.72;
          v = Math.max(14, Math.min(238, v)) | 0;
          ctx.fillStyle = 'rgb(' + v + ',' + v + ',' + v + ')';
          ctx.fillRect(x0 + 26 + x, y, 4, 2);
        }
        const s = (y >> 1) % 2 === 0 ? 225 : 18;
        ctx.fillStyle = 'rgb(' + s + ',' + s + ',' + s + ')';
        ctx.fillRect(x0, y, 18, 2);
        const w = 40 + ((y / 32) | 0) % 8 * 26;
        ctx.fillStyle = 'rgb(' + w + ',' + w + ',' + w + ')';
        ctx.fillRect(x0 + 300, y, 22, 2);
      }
    };
    chan(18, false); chan(378, true);
    ctx.strokeStyle = 'rgba(61,245,140,.5)'; ctx.setLineDash([6, 5]);
    ctx.beginPath(); ctx.moveTo(0, dec + 1.5); ctx.lineTo(W, dec + 1.5); ctx.stroke();
    ctx.fillStyle = '#6a6252'; ctx.font = '11px "Fragment Mono", monospace';
    ctx.fillText('DECODING\u2026 LINE 274 / 440', 18, dec + 22);
    ctx.fillStyle = '#3df58c'; ctx.font = '10px "Fragment Mono", monospace';
    ctx.fillText('CH A \u00b7 VIS', 18, 14); ctx.fillText('CH B \u00b7 IR', 378, 14);
  }
  open(a) {
    clearInterval(this.fftT); clearInterval(this.feedT); this.fftT = this.feedT = null;
    const view = a.v || 'screen';
    this.setState({ view, sel: a, captured: false, fft: null, peak: null });
    if (view === 'spectrum') this.startFft();
    if (view === 'receiver') this.startRx();
    if (view === 'console') this.startConsole();
    if (view === 'table') this.startTable(a.nm);
    if (view === 'image') setTimeout(() => this.paintApt(), 80);
  }
  renderVals() {
    const s = this.state;
    const ic = (k) => (this.icons ? this.icons[k]() : null);
    const ro = this.props.receiveOnly ?? false;
    const motion = this.props.motion ?? 'full';
    const q = s.q.trim().toLowerCase();
    const age = (ts) => Math.max(0, Math.round((s.now - ts) / 1000)) + 's';
    let shown = 0;
    const chips = [['All', 94], ['RECEIVE', 20], ['TRANSMIT', 28], ['CAPTURE', 3], ['SEARCH', 5], ['UTILITIES', 18], ['GAMES', 7], ['SETTINGS', 13]].map((c) => {
      const act = s.cat === c[0];
      return { t: c[0] + ' ' + c[1], on: () => this.setState({ cat: c[0] }), bg: act ? 'rgba(61,245,140,.12)' : 'transparent', bd: act ? 'rgba(61,245,140,.55)' : '#2b2820', cl: act ? '#3df58c' : '#a09681' };
    });
    const sections = this.D.map((sec) => {
      const active = s.cat === 'All' || s.cat === sec[1];
      const apps = !active ? [] : sec[3].filter((e) => !q || e[0].toLowerCase().includes(q)).map((e) => {
        const tx = (e[2] || '').includes('t'), live = (e[2] || '').includes('l'), dis = ro && tx;
        return {
          nm: e[0], el: ic(e[1]), nat: (e[2] || '').includes('n'), tx, live, dis,
          txt: dis ? 'TX LOCKED' : 'TX',
          tint: live ? '#3df58c' : '#b3a98f',
          bd: live ? 'rgba(61,245,140,.4)' : '#242019',
          op: dis ? '0.38' : '1',
          open: () => this.open({ nm: e[0], i: e[1], cat: sec[1], v: e[3] })
        };
      });
      shown += apps.length;
      return { ix: sec[0], label: sec[1], n: sec[3].length, el: ic(sec[2]), apps, has: apps.length > 0 };
    }).filter((x) => x.has);
    const rows = this.acs.map((a, i) => ({
      cs: a.cs || '', ic: a.ic,
      alt: a.alt != null ? a.alt.toLocaleString('en-US') : '',
      gs: a.gs != null ? String(a.gs) : '', hd: a.hd != null ? a.hd + '\u00b0' : '',
      age: age(a.ts), nofix: a.x == null, x: a.x, y: a.y,
      rot: 'rotate(' + (a.hd || 0) + 'deg)',
      fl: a.alt != null ? 'FL' + Math.round(a.alt / 100) : '',
      sel: s.selAc === i,
      col: s.selAc === i ? '#ff9438' : '#3df58c',
      rowBg: s.selAc === i ? 'rgba(255,148,56,.07)' : 'transparent',
      rowOp: a.x == null ? '0.55' : '1',
      on: () => this.setState({ selAc: i })
    }));
    const shipRows = this.ships.map((a, i) => ({
      nm: a.nm || '', mm: a.mm, sog: a.sog.toFixed(1), cog: a.cog + '\u00b0', ty: a.ty || '',
      kts: a.sog.toFixed(1) + ' kt', age: age(a.ts), x: a.x, y: a.y,
      rot: 'rotate(' + a.cog + 'deg)', sel: s.selShip === i,
      col: s.selShip === i ? '#ff9438' : '#3df58c',
      rowBg: s.selShip === i ? 'rgba(255,148,56,.07)' : 'transparent',
      on: () => this.setState({ selShip: i })
    }));
    let fftPath = '', fftArea = '', peakPath = '';
    if (s.fft) {
      const N = s.fft.length;
      const pt = (v, i) => ((i / (N - 1)) * 512).toFixed(1) + ' ' + (((-23 - v) / 70) * 190 + 4).toFixed(1);
      fftPath = 'M' + s.fft.map(pt).join(' L');
      fftArea = fftPath + ' L512 200 L0 200 Z';
      peakPath = 'M' + s.peak.map(pt).join(' L');
    }
    const isRecon = s.sel && s.sel.nm === 'Recon';
    const R = { al: 'right' }, L = { al: 'left' };
    const tblCols = isRecon
      ? [{ t: 'FREQ MHZ', al: 'left' }, { t: 'MODE', al: 'left' }, { t: 'HITS', al: 'right' }, { t: 'PEAK DB', al: 'right' }, { t: 'LAST', al: 'right' }]
      : [{ t: 'TIME', al: 'left' }, { t: 'ADDR', al: 'left' }, { t: 'FN', al: 'left' }, { t: 'TYPE', al: 'left' }, { t: 'MESSAGE', al: 'left' }];
    const tblRows = isRecon
      ? this.reconRows.map((r) => ({ an: r.fresh ? 'mpRowIn 1.4s ease both' : 'none', cells: [{ t: r.f, cl: '#ede8db', al: 'left' }, { t: r.mode, cl: '#a09681', al: 'left' }, { t: String(r.hits), cl: '#3df58c', al: 'right' }, { t: String(r.db), cl: '#cfc7b4', al: 'right' }, { t: age(r.ts), cl: '#6a6252', al: 'right' }] }))
      : this.pocsagRows.map((r) => ({ an: r.fresh ? 'mpRowIn 1.4s ease both' : 'none', cells: [{ t: r.tm, cl: '#6a6252', al: 'left' }, { t: r.addr, cl: '#ede8db', al: 'left' }, { t: r.fn, cl: '#b06a2a', al: 'left' }, { t: r.ty, cl: '#a09681', al: 'left' }, { t: r.msg, cl: '#cfc7b4', al: 'left' }] }));
    const el = (s.now - this.t0) / 1000;
    const sndAltN = 12480 + el * 4.8;
    const smDbV = Math.round(s.sm);
    const smSegs = Array.from({ length: 24 }, (_, i) => {
      const thr = -100 + (i * 70) / 24;
      const on = s.sm >= thr;
      return { c: !on ? '#1d1a14' : i < 16 ? '#3df58c' : i < 20 ? '#ff9438' : '#ff5c5c' };
    });
    const scrLabels = [['Receive', '#3ddc84'], ['Transmit', '#ef5464'], ['Capture', '#f5a623'], ['Replay', '#f5a623'], ['Search', '#5b9dff'], ['Recon', '#5b9dff'], ['Level', '#3ddc84'], ['Scanner', '#5b9dff'], ['Mic', '#ef5464'], ['Utilities', '#dd3'], ['Games', '#a76bff'], ['Settings', '#9a9a9a'], ['Debug', '#9a9a9a'], ['HackRF', '#3ddc84'], ['File Man', '#dd3'], ['About', '#9a9a9a']];
    const scrTiles = scrLabels.map((t, i) => ({ l: t[0], c: t[1], ol: i === 5 ? '1px solid #fff' : 'none', an: i === 5 ? 'mpBlink 1.2s step-end infinite' : 'none' }));
    const cap = s.captured;
    return {
      motionVars: { '--ics': motion === 'calm' ? '2.6' : '1', '--icp': motion === 'off' ? 'paused' : 'running' },
      clock: new Date(s.now).toISOString().slice(11, 19) + 'Z',
      txT: ro ? 'TX LOCKED' : 'TX READY', txC: ro ? '#ff9438' : '#6a6252',
      isGrid: s.view === 'grid', isAdsb: s.view === 'adsb', isAis: s.view === 'ais', isSpectrum: s.view === 'spectrum', isReceiver: s.view === 'receiver', isConsole: s.view === 'console', isTable: s.view === 'table', isImage: s.view === 'image', isSonde: s.view === 'sonde', isScreen: s.view === 'screen',
      q: s.q, onSearch: (e) => this.setState({ q: e.target.value }), clearQ: () => this.setState({ q: '' }),
      chips, sections, shown, noResults: shown === 0, emptyEl: ic('search'),
      back: () => { clearInterval(this.fftT); clearInterval(this.feedT); this.fftT = this.feedT = null; this.setState({ view: 'grid' }); },
      adsbIco: ic('adsb'), specIco: ic('spectrum'), aisIco: ic('ais'), audIco: ic('audio'), acarsIco: ic('acars'), aptIco: ic('sstv'), sondeIco: ic('sonde'), sondeMk: ic('sonde'),
      rows, markers: rows.filter((r) => !r.nofix),
      acCount: rows.filter((r) => !r.nofix).length, noFix: rows.filter((r) => r.nofix).length,
      msgRate: 31 + Math.round(Math.sin(s.now / 2800) * 5),
      shipRows, shipMarkers: shipRows, shipCount: shipRows.length,
      hasFft: !!s.fft, noFft: !s.fft, fftPath, fftArea, peakPath, wfRef: this.wfRef, apRef: this.apRef,
      sqlT: s.sqlOpen ? 'OPEN' : 'CLOSED', sqlC: s.sqlOpen ? '#3df58c' : '#6a6252',
      rxFreq: (s.rxk / 1000).toFixed(3),
      fUp: () => this.setState({ rxk: s.rxk + 100 }), fDown: () => this.setState({ rxk: s.rxk - 100 }),
      smSegs, smDb: smDbV + ' dBm', smSu: smDbV > -73 ? 'S9+' + Math.max(0, smDbV + 73) : 'S' + Math.max(1, Math.round((smDbV + 127) / 6)),
      vuW: s.vu.toFixed(0),
      logLines: this.logs.map((l) => ({ tm: l.tm, fl: l.fl, lab: l.lab, dir: l.dir, dc: l.dir === '\u2191' ? '#ff9438' : '#3df58c', txt: l.txt || '(no text)', an: l.fresh ? 'mpRowIn 1.4s ease both' : 'none' })),
      logRate: 26 + Math.round(Math.sin(s.now / 4000) * 5), clearLog: () => { this.logs = []; this.forceUpdate(); },
      tblGrid: isRecon ? '1.2fr .7fr .6fr .7fr .6fr' : '.85fr .8fr .4fr .8fr 2.4fr',
      tblSub: isRecon ? 'SCAN LIST \u00b7 7 ENTRIES' : '466.175 MHz \u00b7 1200 BD',
      tblNote: isRecon ? 'Hits accumulate per scan pass; PEAK DB is the strongest verified hit, never an estimate.' : 'TONE pages carry no text \u2014 the MESSAGE cell stays empty rather than inventing one.',
      tblCols, tblRows, tblCount: tblRows.length,
      sndAlt: Math.round(sndAltN).toLocaleString('en-US').replace(/,/g, '\u2009'),
      sndClimb: (4.8 + Math.sin(el / 6) * 0.5).toFixed(1),
      sndTemp: Math.max(-56.5, 15 - 6.5 * (sndAltN / 1000)).toFixed(1),
      sndFrame: (3120 + el) | 0,
      sndX: Math.min(70, 52 + el * 0.03).toFixed(2), sndY: Math.max(16, 34 - el * 0.02).toFixed(2),
      sndTrail: [{ x: 24, y: 78, op: '.25' }, { x: 29, y: 72, op: '.32' }, { x: 34, y: 65, op: '.4' }, { x: 38, y: 58, op: '.5' }, { x: 43, y: 50, op: '.6' }, { x: 47, y: 44, op: '.72' }, { x: 50, y: 39, op: '.85' }],
      selName: s.sel ? s.sel.nm : '', selNameUp: s.sel ? s.sel.nm.toUpperCase() : '', selCat: s.sel ? s.sel.cat : '',
      selEl: s.sel ? ic(s.sel.i) : null, selEl2: s.sel ? ic(s.sel.i) : null,
      scrTiles, onScr: () => this.setState({ captured: !cap }),
      scrBd: cap ? '#3df58c' : '#2b2820',
      capT: cap ? 'INPUT LIVE' : 'INPUT IDLE', capC: cap ? '#3df58c' : '#6a6252', capDot: cap ? '#3df58c' : '#4a4438',
      capBd: cap ? 'rgba(61,245,140,.45)' : '#242019',
      capNote: cap ? '#3df58c' : '#a09681',
      capMsg: cap ? 'Input live \u2014 arrow keys, Enter, Esc, scroll and clicks are forwarded to the device. Click the screen again to release.' : 'Click the screen to capture keyboard and mouse. Nothing is forwarded until you do.'
    };
  }
}
